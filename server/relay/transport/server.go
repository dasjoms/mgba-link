package transport

import (
	"bufio"
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

func NewServer(cfg *config.Config, logger *log.Logger) *Server {
	return &Server{
		cfg:   cfg,
		log:   logger,
		rooms: rooms.NewManager(cfg.MaxRooms, cfg.MaxPlayersPerRoom),
	}
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

func (s *Server) handleConn(conn net.Conn) {
	id := atomic.AddUint64(&s.nextConnID, 1)
	sessionID := fmt.Sprintf("c-%d", id)
	loggerPrefix := fmt.Sprintf("%s %s", conn.RemoteAddr().String(), sessionID)
	s.log.Printf("connected %s", loggerPrefix)
	defer func() {
		s.log.Printf("disconnected %s", loggerPrefix)
		_ = conn.Close()
	}()

	reader := bufio.NewReaderSize(conn, s.cfg.ClientReadBufSize)
	session := &rooms.Session{ID: sessionID, Send: make(chan []byte, 32)}
	roomID := ""
	defer close(session.Send)
	defer func() {
		if roomID != "" {
			s.rooms.Leave(roomID, session.ID)
		}
	}()

	heartbeatTicker := time.NewTicker(s.cfg.HeartbeatInterval)
	defer heartbeatTicker.Stop()
	lastPong := time.Now()

	go func() {
		for b := range session.Send {
			_ = conn.SetWriteDeadline(time.Now().Add(s.cfg.ClientWriteTimeout))
			_, _ = conn.Write(b)
		}
	}()

	for {
		_ = conn.SetReadDeadline(time.Now().Add(s.cfg.HeartbeatTimeout))
		msg, err := protocol.ReadMessage(reader)
		if err != nil {
			return
		}

		switch msg.Type {
		case "join":
			if s.cfg.Secret != "" && msg.Secret != s.cfg.Secret {
				_ = protocol.WriteJSONLine(conn, map[string]any{"type": "error", "error": "unauthorized"})
				return
			}
			session.Player = msg.Player
			if err := s.rooms.Join(msg.RoomID, session); err != nil {
				_ = protocol.WriteJSONLine(conn, map[string]any{"type": "error", "error": err.Error()})
				return
			}
			roomID = msg.RoomID
			_ = protocol.WriteJSONLine(conn, map[string]any{"type": "joined", "roomId": roomID, "session": session.ID})
		case "leave":
			if roomID != "" {
				s.rooms.Leave(roomID, session.ID)
			}
			return
		case "pong":
			lastPong = time.Now()
		case "message":
			if roomID == "" {
				_ = protocol.WriteJSONLine(conn, map[string]any{"type": "error", "error": "must join room first"})
				continue
			}
			env := protocol.Envelope{From: session.Player, Type: "message", Payload: msg.Payload}
			payload, _ := json.Marshal(env)
			payload = append(payload, '\n')
			s.rooms.Broadcast(roomID, session.ID, payload)
		default:
			_ = protocol.WriteJSONLine(conn, map[string]any{"type": "error", "error": "unknown message type"})
		}

		select {
		case <-heartbeatTicker.C:
			if time.Since(lastPong) > s.cfg.HeartbeatTimeout {
				_ = protocol.WriteJSONLine(conn, map[string]any{"type": "error", "error": "heartbeat timeout"})
				if roomID != "" {
					s.rooms.Leave(roomID, session.ID)
				}
				return
			}
			_ = protocol.WriteJSONLine(conn, map[string]any{"type": "ping", "ts": time.Now().UnixNano()})
		default:
		}
	}
}
