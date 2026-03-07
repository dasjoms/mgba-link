package transport

import (
	"encoding/json"
	"errors"
	"fmt"
	"log"
	"net"
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
	defer close(session.Send)
	defer func() {
		if roomID != "" {
			s.leaveRoomAndNotifyPeers(roomID, session.ID, playerID, "clientRequested")
		}
		state = stateDisconnected
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
				s.writeProtocolViolationAndDisconnect(conn, &protocol.ProtocolViolation{Code: timeoutCode, Message: "heartbeat timeout", Reason: "networkTimeout"})
				if roomID != "" {
					s.leaveRoomAndNotifyPeers(roomID, session.ID, playerID, "networkTimeout")
					roomID = ""
					playerID = 0
				}
				return
			}
			if v, ok := err.(*protocol.ProtocolViolation); ok {
				s.writeProtocolViolationAndDisconnect(conn, v)
			}
			return
		}

		intent, violation := protocol.ParseAndValidateClientIntent(payload)
		if violation != nil {
			s.writeProtocolViolationAndDisconnect(conn, violation)
			return
		}

		switch intent.Intent {
		case "hello":
			if state != stateConnected {
				s.writeProtocolViolationAndDisconnect(conn, &protocol.ProtocolViolation{Code: 409, Message: "hello already completed", Reason: "protocolError"})
				return
			}
			if s.cfg.Secret != "" && intent.AuthToken != s.cfg.Secret {
				s.writeProtocolViolationAndDisconnect(conn, &protocol.ProtocolViolation{Code: 401, Message: "access denied", Reason: "protocolError"})
				return
			}
			state = stateHandshaken
			_ = writeEvent(conn, "playerAssigned", map[string]any{"playerId": playerID, "displayName": session.ID, "roomId": roomID})
		case "createRoom":
			if state == stateConnected {
				s.writeProtocolViolationAndDisconnect(conn, &protocol.ProtocolViolation{Code: 400, Message: "hello required before createRoom", Reason: "protocolError"})
				return
			}
			if roomID != "" {
				s.writeProtocolViolationAndDisconnect(conn, &protocol.ProtocolViolation{Code: 409, Message: "already joined room", Reason: "protocolError"})
				return
			}
			if err := s.rooms.Create(intent.RoomName, intent.MaxPlayers); err != nil {
				s.writeProtocolViolationAndDisconnect(conn, &protocol.ProtocolViolation{Code: 403, Message: fmt.Sprintf("room create denied: %s", err.Error()), Reason: "protocolError"})
				return
			}
			session.Player = session.ID
			assignedPlayerID, memberCount, err := s.rooms.Join(intent.RoomName, session)
			if err != nil {
				s.writeProtocolViolationAndDisconnect(conn, &protocol.ProtocolViolation{Code: 500, Message: fmt.Sprintf("room join after create failed: %s", err.Error()), Reason: "protocolError"})
				return
			}
			roomID = intent.RoomName
			playerID = assignedPlayerID
			state = stateRoomJoined
			_ = writeEvent(conn, "playerAssigned", map[string]any{"playerId": playerID, "displayName": session.ID, "roomId": roomID})
			_ = writeEvent(conn, "roomJoined", map[string]any{"roomId": roomID, "roomName": intent.RoomName, "maxPlayers": intent.MaxPlayers, "memberCount": memberCount})
		case "joinRoom":
			if state == stateConnected {
				s.writeProtocolViolationAndDisconnect(conn, &protocol.ProtocolViolation{Code: 400, Message: "hello required before joinRoom", Reason: "protocolError"})
				return
			}
			if roomID != "" {
				s.writeProtocolViolationAndDisconnect(conn, &protocol.ProtocolViolation{Code: 409, Message: "already joined room", Reason: "protocolError"})
				return
			}
			session.Player = session.ID
			assignedPlayerID, memberCount, err := s.rooms.Join(intent.RoomID, session)
			if err != nil {
				code := 403
				if errors.Is(err, rooms.ErrRoomNotFound) {
					code = 404
				}
				s.writeProtocolViolationAndDisconnect(conn, &protocol.ProtocolViolation{Code: code, Message: fmt.Sprintf("room join denied: %s", err.Error()), Reason: "protocolError"})
				return
			}
			roomID = intent.RoomID
			playerID = assignedPlayerID
			state = stateRoomJoined
			_ = writeEvent(conn, "playerAssigned", map[string]any{"playerId": playerID, "displayName": session.ID, "roomId": roomID})
			_ = writeEvent(conn, "roomJoined", map[string]any{"roomId": roomID, "roomName": roomID, "maxPlayers": s.cfg.MaxPlayersPerRoom, "memberCount": memberCount})
		case "leaveRoom":
			if state == stateConnected {
				s.writeProtocolViolationAndDisconnect(conn, &protocol.ProtocolViolation{Code: 400, Message: "hello required before leaveRoom", Reason: "protocolError"})
				return
			}
			if roomID != "" {
				s.leaveRoomAndNotifyPeers(roomID, session.ID, playerID, "clientRequested")
				roomID = ""
				playerID = 0
				state = stateHandshaken
			}
			_ = writeEvent(conn, "disconnected", map[string]any{"reason": "clientRequested", "message": "left room"})
			state = stateDisconnected
			return
		case "heartbeat":
			if state == stateConnected {
				s.writeProtocolViolationAndDisconnect(conn, &protocol.ProtocolViolation{Code: 400, Message: "hello required before heartbeat", Reason: "protocolError"})
				return
			}
			lastHeartbeat = time.Now()
			_ = writeEvent(conn, "heartbeatAck", map[string]any{"heartbeatCounter": intent.Heartbeat, "roomId": roomID})
		case "publishLinkEvent":
			if state == stateConnected {
				s.writeProtocolViolationAndDisconnect(conn, &protocol.ProtocolViolation{Code: 400, Message: "hello required before publishLinkEvent", Reason: "protocolError"})
				return
			}
			if roomID == "" {
				s.writeProtocolViolationAndDisconnect(conn, &protocol.ProtocolViolation{Code: 403, Message: "must join room first", Reason: "protocolError"})
				return
			}
			var event protocol.LinkEvent
			if err := json.Unmarshal(intent.Event, &event); err != nil {
				s.writeProtocolViolationAndDisconnect(conn, &protocol.ProtocolViolation{Code: 400, Message: "invalid event payload", Reason: "protocolError"})
				return
			}

			serverSequence, senderPlayerID, participants, err := s.rooms.ReservePublish(roomID, session.ID, event.Sequence)
			if err != nil {
				code := 403
				if errors.Is(err, rooms.ErrEventSequenceViolation) {
					code = 409
				}
				s.writeProtocolViolationAndDisconnect(conn, &protocol.ProtocolViolation{Code: code, Message: err.Error(), Reason: "protocolError"})
				return
			}
			event.SenderPlayerID = senderPlayerID
			out, _ := protocol.MarshalEvent("inboundLinkEvent", map[string]any{"roomId": roomID, "serverSequence": serverSequence, "event": event})
			rooms.BroadcastOrdered(participants, out)
		default:
			s.writeProtocolViolationAndDisconnect(conn, &protocol.ProtocolViolation{Code: 400, Message: "unknown intent", Reason: "protocolError"})
			return
		}

		select {
		case <-heartbeatTicker.C:
			if time.Since(lastHeartbeat) > s.cfg.HeartbeatTimeout {
				timeoutCode := timeoutCodeForDuration(s.cfg.HeartbeatTimeout)
				s.writeProtocolViolationAndDisconnect(conn, &protocol.ProtocolViolation{Code: timeoutCode, Message: "heartbeat timeout", Reason: "networkTimeout"})
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
