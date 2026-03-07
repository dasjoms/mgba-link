package transport

import (
	"encoding/json"
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
	state := stateConnected
	defer close(session.Send)
	defer func() {
		if roomID != "" {
			s.rooms.Leave(roomID, session.ID)
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
			_ = writeEvent(conn, "playerAssigned", map[string]any{"playerId": 1, "displayName": session.ID, "roomId": roomID})
		case "createRoom":
			if state == stateConnected {
				s.writeProtocolViolationAndDisconnect(conn, &protocol.ProtocolViolation{Code: 400, Message: "hello required before createRoom", Reason: "protocolError"})
				return
			}
			session.Player = session.ID
			if err := s.rooms.Join(intent.RoomName, session); err != nil {
				s.writeProtocolViolationAndDisconnect(conn, &protocol.ProtocolViolation{Code: 403, Message: fmt.Sprintf("room join denied: %s", err.Error()), Reason: "protocolError"})
				return
			}
			roomID = intent.RoomName
			state = stateRoomJoined
			_ = writeEvent(conn, "roomJoined", map[string]any{"roomId": roomID, "roomName": intent.RoomName, "maxPlayers": intent.MaxPlayers})
		case "joinRoom":
			if state == stateConnected {
				s.writeProtocolViolationAndDisconnect(conn, &protocol.ProtocolViolation{Code: 400, Message: "hello required before joinRoom", Reason: "protocolError"})
				return
			}
			session.Player = session.ID
			if err := s.rooms.Join(intent.RoomID, session); err != nil {
				s.writeProtocolViolationAndDisconnect(conn, &protocol.ProtocolViolation{Code: 403, Message: fmt.Sprintf("room join denied: %s", err.Error()), Reason: "protocolError"})
				return
			}
			roomID = intent.RoomID
			state = stateRoomJoined
			_ = writeEvent(conn, "roomJoined", map[string]any{"roomId": roomID, "roomName": roomID, "maxPlayers": s.cfg.MaxPlayersPerRoom})
		case "leaveRoom":
			if state == stateConnected {
				s.writeProtocolViolationAndDisconnect(conn, &protocol.ProtocolViolation{Code: 400, Message: "hello required before leaveRoom", Reason: "protocolError"})
				return
			}
			if roomID != "" {
				s.rooms.Leave(roomID, session.ID)
				roomID = ""
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
			var event map[string]any
			_ = json.Unmarshal(intent.Event, &event)
			out, _ := protocol.MarshalEvent("inboundLinkEvent", map[string]any{"roomId": roomID, "event": event})
			s.rooms.Broadcast(roomID, session.ID, out)
		default:
			s.writeProtocolViolationAndDisconnect(conn, &protocol.ProtocolViolation{Code: 400, Message: "unknown intent", Reason: "protocolError"})
			return
		}

		select {
		case <-heartbeatTicker.C:
			if time.Since(lastHeartbeat) > s.cfg.HeartbeatTimeout {
				s.writeProtocolViolationAndDisconnect(conn, &protocol.ProtocolViolation{Code: 400, Message: "heartbeat timeout", Reason: "networkTimeout"})
				if roomID != "" {
					s.rooms.Leave(roomID, session.ID)
				}
				return
			}
		default:
		}
	}
}
