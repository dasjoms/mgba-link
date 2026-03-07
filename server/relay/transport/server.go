package transport

import (
	"encoding/json"
	"errors"
	"fmt"
	"log"
	"net"
	"strings"
	"sync/atomic"
	"time"

	"mgba-link-relay/config"
	"mgba-link-relay/protocol"
	"mgba-link-relay/rooms"
)

type Server struct {
	cfg        *config.Config
	log        *log.Logger
	rooms      *rooms.Manager
	nextConnID uint64
}

type connState int

const (
	stateConnected connState = iota
	stateHandshaken
	stateRoomJoined
	stateDisconnected
)

func NewServer(cfg *config.Config, logger *log.Logger) *Server {
	return &Server{cfg: cfg, log: logger, rooms: rooms.NewManager(cfg.MaxRooms, cfg.MaxPlayersPerRoom)}
}

func (s *Server) ListenAndServe() error {
	ln, err := net.Listen("tcp", s.cfg.Addr())
	if err != nil {
		return fmt.Errorf("listen: %w", err)
	}
	s.log.Printf("listening on %s", s.cfg.Addr())
	for {
		conn, err := ln.Accept()
		if err != nil {
			s.log.Printf("accept error: %v", err)
			continue
		}
		go s.handleConn(conn)
	}
}

func writeEvent(conn net.Conn, kind string, fields map[string]any) error {
	payload, err := protocol.MarshalEvent(kind, fields)
	if err != nil {
		return err
	}
	return protocol.WriteFrame(conn, payload)
}

func stateName(state connState) string {
	switch state {
	case stateConnected:
		return "connected"
	case stateHandshaken:
		return "handshaken"
	case stateRoomJoined:
		return "roomJoined"
	case stateDisconnected:
		return "disconnected"
	default:
		return "unknown"
	}
}

func redactValue(value any) any {
	switch v := value.(type) {
	case map[string]any:
		out := make(map[string]any, len(v))
		for key, child := range v {
			if shouldRedactField(key) {
				out[key] = "[REDACTED]"
				continue
			}
			out[key] = redactValue(child)
		}
		return out
	case []any:
		out := make([]any, 0, len(v))
		for _, child := range v {
			out = append(out, redactValue(child))
		}
		return out
	default:
		return value
	}
}

func extractClientSequence(payload map[string]any) int64 {
	if payload == nil {
		return 0
	}
	if v, ok := payload["clientSequence"].(float64); ok {
		return int64(v)
	}
	return 0
}

func shouldRedactField(field string) bool {
	lower := strings.ToLower(field)
	return strings.Contains(lower, "auth") || strings.Contains(lower, "token") || strings.Contains(lower, "secret")
}

func (s *Server) logStructured(fields map[string]any) {
	if fields == nil {
		fields = map[string]any{}
	}
	safe := redactValue(fields)
	b, err := json.Marshal(safe)
	if err != nil {
		s.log.Printf("logMarshalError err=%v", err)
		return
	}
	s.log.Print(string(b))
}

func (s *Server) writeProtocolViolationAndDisconnect(conn net.Conn, v *protocol.ProtocolViolation) {
	if v == nil {
		return
	}
	_ = writeEvent(conn, "error", map[string]any{"code": v.Code, "message": v.Message})
	reason := v.Reason
	if reason == "" {
		reason = "protocolError"
	}
	_ = writeEvent(conn, "disconnected", map[string]any{"reason": reason, "message": v.Message})
}

func heartbeatHealthFields(lastHeartbeat time.Time, timeout time.Duration) map[string]any {
	ageMs := time.Since(lastHeartbeat).Milliseconds()
	if ageMs < 0 {
		ageMs = 0
	}
	timeoutMs := timeout.Milliseconds()
	if timeoutMs < 0 {
		timeoutMs = 0
	}
	return map[string]any{
		"heartbeatAgeMs":     ageMs,
		"heartbeatTimeoutMs": timeoutMs,
		"heartbeatHealthy":   time.Since(lastHeartbeat) <= timeout,
	}
}

func timeoutCodeForDuration(d time.Duration) int {
	if d <= 0 {
		return 400
	}
	if d < time.Second {
		return 408
	}
	seconds := int(d / time.Second)
	if seconds < 1 {
		seconds = 1
	}
	if seconds > 999 {
		seconds = 999
	}
	return 400000 + seconds
}

func (s *Server) leaveRoomAndNotifyPeers(roomID, sessionID string, playerID int, reason string) {
	if roomID == "" {
		return
	}
	leftPlayerID, participants := s.rooms.LeaveWithParticipants(roomID, sessionID)
	if leftPlayerID == 0 {
		leftPlayerID = playerID
	}
	if leftPlayerID == 0 {
		return
	}
	if reason == "" {
		reason = "protocolError"
	}
	peerLeft, err := protocol.MarshalEvent("peerLeft", map[string]any{"roomId": roomID, "playerId": leftPlayerID, "reason": reason})
	if err != nil {
		return
	}
	rooms.BroadcastOrdered(participants, peerLeft)
}

func (s *Server) handleConn(conn net.Conn) {
	id := atomic.AddUint64(&s.nextConnID, 1)
	sessionID := fmt.Sprintf("c-%d", id)
	loggerPrefix := fmt.Sprintf("%s %s", conn.RemoteAddr().String(), sessionID)
	s.log.Printf("connected %s", loggerPrefix)
	defer func() {
		s.log.Printf("disconnected %s", loggerPrefix)
		_ = conn.Close()
	}()

	session := &rooms.Session{ID: sessionID, Send: make(chan []byte, 32)}
	roomID := ""
	playerID := 0
	state := stateConnected
	roomServerSequence := int64(0)
	logBase := map[string]any{"sessionId": session.ID, "remoteAddr": conn.RemoteAddr().String()}
	setState := func(next connState, trigger string) {
		if state == next {
			return
		}
		s.logStructured(map[string]any{
			"event":       "stateTransition",
			"fromState":   stateName(state),
			"toState":     stateName(next),
			"trigger":     trigger,
			"roomId":      roomID,
			"playerId":    playerID,
			"sessionId":   logBase["sessionId"],
			"remoteAddr":  logBase["remoteAddr"],
			"state":       stateName(next),
			"messageKind": trigger,
		})
		state = next
	}
	logMessage := func(direction string, kind string, clientSeq int64, serverSeq int64, extra map[string]any) {
		entry := map[string]any{
			"event":              "message",
			"direction":          direction,
			"messageKind":        kind,
			"roomId":             roomID,
			"playerId":           playerID,
			"clientSequence":     clientSeq,
			"serverSequence":     serverSeq,
			"roomServerSequence": roomServerSequence,
			"sessionId":          logBase["sessionId"],
			"remoteAddr":         logBase["remoteAddr"],
			"state":              stateName(state),
		}
		for k, v := range extra {
			entry[k] = v
		}
		s.logStructured(entry)
	}
	logProtocolViolation := func(v *protocol.ProtocolViolation) {
		if v == nil {
			return
		}
		s.logStructured(map[string]any{
			"event":                 "protocolViolation",
			"code":                  v.Code,
			"category":              v.Reason,
			"message":               v.Message,
			"roomId":                roomID,
			"playerId":              playerID,
			"sessionId":             logBase["sessionId"],
			"remoteAddr":            logBase["remoteAddr"],
			"state":                 stateName(state),
			"protocolViolation":     true,
			"protocolViolationCode": v.Code,
		})
	}
	sendEvent := func(kind string, fields map[string]any, clientSeq int64, serverSeq int64) error {
		logMessage("outbound", kind, clientSeq, serverSeq, fields)
		return writeEvent(conn, kind, fields)
	}
	defer close(session.Send)
	defer func() {
		if roomID != "" {
			s.leaveRoomAndNotifyPeers(roomID, session.ID, playerID, "clientRequested")
		}
		setState(stateDisconnected, "connectionClosed")
	}()

	heartbeatTicker := time.NewTicker(s.cfg.HeartbeatInterval)
	defer heartbeatTicker.Stop()
	lastHeartbeat := time.Now()

	go func() {
		for b := range session.Send {
			_ = conn.SetWriteDeadline(time.Now().Add(s.cfg.ClientWriteTimeout))
			_ = protocol.WriteFrame(conn, b)
		}
	}()

	for {
		_ = conn.SetReadDeadline(time.Now().Add(s.cfg.HeartbeatTimeout))
		payload, err := protocol.ReadFrame(conn)
		if err != nil {
			if ne, ok := err.(net.Error); ok && ne.Timeout() {
				timeoutCode := timeoutCodeForDuration(s.cfg.HeartbeatTimeout)
				v := &protocol.ProtocolViolation{Code: timeoutCode, Message: "heartbeat timeout", Reason: "networkTimeout"}
				logProtocolViolation(v)
				s.logStructured(map[string]any{"event": "heartbeatHealth", "roomId": roomID, "playerId": playerID, "sessionId": logBase["sessionId"], "remoteAddr": logBase["remoteAddr"], "state": stateName(state), "roomServerSequence": roomServerSequence, "reason": "readTimeout", "health": heartbeatHealthFields(lastHeartbeat, s.cfg.HeartbeatTimeout)})
				s.writeProtocolViolationAndDisconnect(conn, v)
				if roomID != "" {
					s.leaveRoomAndNotifyPeers(roomID, session.ID, playerID, "networkTimeout")
					roomID = ""
					playerID = 0
				}
				return
			}
			if v, ok := err.(*protocol.ProtocolViolation); ok {
				logProtocolViolation(v)
				s.logStructured(map[string]any{"event": "heartbeatHealth", "roomId": roomID, "playerId": playerID, "sessionId": logBase["sessionId"], "remoteAddr": logBase["remoteAddr"], "state": stateName(state), "roomServerSequence": roomServerSequence, "reason": "readViolation", "health": heartbeatHealthFields(lastHeartbeat, s.cfg.HeartbeatTimeout)})
				s.writeProtocolViolationAndDisconnect(conn, v)
			}
			return
		}

		var inbound map[string]any
		if err := json.Unmarshal(payload, &inbound); err == nil {
			logMessage("inbound", fmt.Sprintf("%v", inbound["intent"]), extractClientSequence(inbound), 0, map[string]any{"payload": inbound})
		}

		intent, violation := protocol.ParseAndValidateClientIntent(payload)
		if violation != nil {
			logProtocolViolation(violation)
			s.writeProtocolViolationAndDisconnect(conn, violation)
			return
		}

		switch intent.Intent {
		case "hello":
			if state != stateConnected {
				v := &protocol.ProtocolViolation{Code: 409, Message: "hello already completed", Reason: "protocolError"}
				logProtocolViolation(v)
				s.writeProtocolViolationAndDisconnect(conn, v)
				return
			}
			if s.cfg.Secret != "" && intent.AuthToken != s.cfg.Secret {
				v := &protocol.ProtocolViolation{Code: 401, Message: "access denied", Reason: "protocolError"}
				logProtocolViolation(v)
				s.writeProtocolViolationAndDisconnect(conn, v)
				return
			}
			setState(stateHandshaken, "hello")
			_ = sendEvent("playerAssigned", map[string]any{"playerId": playerID, "displayName": session.ID, "roomId": roomID}, intent.ClientSequence, 0)
		case "createRoom":
			if state == stateConnected {
				v := &protocol.ProtocolViolation{Code: 400, Message: "hello required before createRoom", Reason: "protocolError"}
				logProtocolViolation(v)
				s.writeProtocolViolationAndDisconnect(conn, v)
				return
			}
			if roomID != "" {
				v := &protocol.ProtocolViolation{Code: 409, Message: "already joined room", Reason: "protocolError"}
				logProtocolViolation(v)
				s.writeProtocolViolationAndDisconnect(conn, v)
				return
			}
			authoritativeRoomID, err := s.rooms.Create(intent.RoomName, intent.MaxPlayers)
			if err != nil {
				code := 403
				if errors.Is(err, rooms.ErrInvalidRoomID) || errors.Is(err, rooms.ErrInvalidRoomCapacity) || errors.Is(err, rooms.ErrUnableToAllocateRoomID) {
					code = 400
				}
				v := &protocol.ProtocolViolation{Code: code, Message: fmt.Sprintf("room create denied: %s", err.Error()), Reason: "protocolError"}
				logProtocolViolation(v)
				s.writeProtocolViolationAndDisconnect(conn, v)
				return
			}
			session.Player = session.ID
			authoritativeRoomID, assignedPlayerID, memberCount, maxPlayers, err := s.rooms.Join(authoritativeRoomID, session)
			if err != nil {
				v := &protocol.ProtocolViolation{Code: 500, Message: fmt.Sprintf("room join after create failed: %s", err.Error()), Reason: "protocolError"}
				logProtocolViolation(v)
				s.writeProtocolViolationAndDisconnect(conn, v)
				return
			}
			roomID = authoritativeRoomID
			playerID = assignedPlayerID
			if !rooms.IsValidPlayerID(playerID) {
				v := &protocol.ProtocolViolation{Code: 500, Message: "invalid assigned player id", Reason: "protocolError"}
				logProtocolViolation(v)
				s.writeProtocolViolationAndDisconnect(conn, v)
				return
			}
			setState(stateRoomJoined, "createRoom")
			_ = sendEvent("playerAssigned", map[string]any{"playerId": playerID, "displayName": session.ID, "roomId": roomID}, intent.ClientSequence, 0)
			_ = sendEvent("roomJoined", map[string]any{"roomId": roomID, "roomName": roomID, "maxPlayers": maxPlayers, "memberCount": memberCount}, intent.ClientSequence, 0)
		case "joinRoom":
			if state == stateConnected {
				v := &protocol.ProtocolViolation{Code: 400, Message: "hello required before joinRoom", Reason: "protocolError"}
				logProtocolViolation(v)
				s.writeProtocolViolationAndDisconnect(conn, v)
				return
			}
			if roomID != "" {
				v := &protocol.ProtocolViolation{Code: 409, Message: "already joined room", Reason: "protocolError"}
				logProtocolViolation(v)
				s.writeProtocolViolationAndDisconnect(conn, v)
				return
			}
			session.Player = session.ID
			authoritativeRoomID, assignedPlayerID, memberCount, maxPlayers, err := s.rooms.Join(intent.RoomID, session)
			if err != nil {
				code := 403
				if errors.Is(err, rooms.ErrRoomNotFound) {
					code = 404
				}
				if errors.Is(err, rooms.ErrInvalidRoomID) || errors.Is(err, rooms.ErrMissingRoomID) {
					code = 400
				}
				v := &protocol.ProtocolViolation{Code: code, Message: fmt.Sprintf("room join denied: %s", err.Error()), Reason: "protocolError"}
				logProtocolViolation(v)
				s.writeProtocolViolationAndDisconnect(conn, v)
				return
			}
			roomID = authoritativeRoomID
			playerID = assignedPlayerID
			if !rooms.IsValidPlayerID(playerID) {
				v := &protocol.ProtocolViolation{Code: 500, Message: "invalid assigned player id", Reason: "protocolError"}
				logProtocolViolation(v)
				s.writeProtocolViolationAndDisconnect(conn, v)
				return
			}
			setState(stateRoomJoined, "joinRoom")
			_ = sendEvent("playerAssigned", map[string]any{"playerId": playerID, "displayName": session.ID, "roomId": roomID}, intent.ClientSequence, 0)
			_ = sendEvent("roomJoined", map[string]any{"roomId": roomID, "roomName": roomID, "maxPlayers": maxPlayers, "memberCount": memberCount}, intent.ClientSequence, 0)
		case "leaveRoom":
			if state == stateConnected {
				v := &protocol.ProtocolViolation{Code: 400, Message: "hello required before leaveRoom", Reason: "protocolError"}
				logProtocolViolation(v)
				s.writeProtocolViolationAndDisconnect(conn, v)
				return
			}
			if roomID != "" {
				s.leaveRoomAndNotifyPeers(roomID, session.ID, playerID, "clientRequested")
				roomID = ""
				playerID = 0
				setState(stateHandshaken, "leaveRoom")
			}
			_ = sendEvent("disconnected", map[string]any{"reason": "clientRequested", "message": "left room"}, intent.ClientSequence, 0)
			setState(stateDisconnected, "leaveRoom")
			return
		case "heartbeat":
			if state == stateConnected {
				v := &protocol.ProtocolViolation{Code: 400, Message: "hello required before heartbeat", Reason: "protocolError"}
				logProtocolViolation(v)
				s.writeProtocolViolationAndDisconnect(conn, v)
				return
			}
			lastHeartbeat = time.Now()
			_ = sendEvent("heartbeatAck", map[string]any{"heartbeatCounter": intent.Heartbeat, "roomId": roomID, "roomServerSequence": roomServerSequence, "health": heartbeatHealthFields(lastHeartbeat, s.cfg.HeartbeatTimeout)}, intent.ClientSequence, 0)
		case "publishLinkEvent":
			if state == stateConnected {
				v := &protocol.ProtocolViolation{Code: 400, Message: "hello required before publishLinkEvent", Reason: "protocolError"}
				logProtocolViolation(v)
				s.writeProtocolViolationAndDisconnect(conn, v)
				return
			}
			if roomID == "" {
				v := &protocol.ProtocolViolation{Code: 403, Message: "must join room first", Reason: "protocolError"}
				logProtocolViolation(v)
				s.writeProtocolViolationAndDisconnect(conn, v)
				return
			}
			var event protocol.LinkEvent
			if err := json.Unmarshal(intent.Event, &event); err != nil {
				v := &protocol.ProtocolViolation{Code: 400, Message: "invalid event payload", Reason: "protocolError"}
				logProtocolViolation(v)
				s.writeProtocolViolationAndDisconnect(conn, v)
				return
			}
			if event.SenderPlayerID != 0 && event.SenderPlayerID != playerID {
				v := &protocol.ProtocolViolation{Code: 409, Message: "event.senderPlayerId must match assigned playerId", Reason: "protocolError"}
				logProtocolViolation(v)
				s.writeProtocolViolationAndDisconnect(conn, v)
				return
			}

			serverSequence, senderPlayerID, participants, err := s.rooms.ReservePublish(roomID, session.ID, event.Sequence)
			if err != nil {
				code := 403
				if errors.Is(err, rooms.ErrEventSequenceViolation) {
					code = 409
				}
				v := &protocol.ProtocolViolation{Code: code, Message: err.Error(), Reason: "protocolError"}
				logProtocolViolation(v)
				s.writeProtocolViolationAndDisconnect(conn, v)
				return
			}
			event.SenderPlayerID = senderPlayerID
			roomServerSequence = serverSequence
			logMessage("outbound", "inboundLinkEvent", intent.ClientSequence, serverSequence, map[string]any{"roomId": roomID, "playerId": senderPlayerID, "roomTick": event.TickMarker, "heartbeat": heartbeatHealthFields(lastHeartbeat, s.cfg.HeartbeatTimeout)})
			out, _ := protocol.MarshalEvent("inboundLinkEvent", map[string]any{"roomId": roomID, "serverSequence": serverSequence, "roomTick": event.TickMarker, "roomServerSequence": roomServerSequence, "event": event})
			rooms.BroadcastOrdered(participants, out)
		default:
			v := &protocol.ProtocolViolation{Code: 400, Message: "unknown intent", Reason: "protocolError"}
			logProtocolViolation(v)
			s.writeProtocolViolationAndDisconnect(conn, v)
			return
		}

		select {
		case <-heartbeatTicker.C:
			if time.Since(lastHeartbeat) > s.cfg.HeartbeatTimeout {
				timeoutCode := timeoutCodeForDuration(s.cfg.HeartbeatTimeout)
				v := &protocol.ProtocolViolation{Code: timeoutCode, Message: "heartbeat timeout", Reason: "networkTimeout"}
				logProtocolViolation(v)
				s.logStructured(map[string]any{"event": "heartbeatHealth", "roomId": roomID, "playerId": playerID, "sessionId": logBase["sessionId"], "remoteAddr": logBase["remoteAddr"], "state": stateName(state), "roomServerSequence": roomServerSequence, "reason": "tickerTimeout", "health": heartbeatHealthFields(lastHeartbeat, s.cfg.HeartbeatTimeout)})
				s.writeProtocolViolationAndDisconnect(conn, v)
				if roomID != "" {
					s.leaveRoomAndNotifyPeers(roomID, session.ID, playerID, "networkTimeout")
					roomID = ""
					playerID = 0
				}
				return
			}
		default:
		}
	}
}
