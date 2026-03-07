package rooms

import (
	"errors"
	"sync"
)

var (
	ErrRoomLimit     = errors.New("room limit reached")
	ErrRoomFull      = errors.New("room is full")
	ErrMissingRoomID = errors.New("room id is required")
)

type Session struct {
	ID     string
	Player string
	Send   chan []byte
}

type Room struct {
	ID       string
	Sessions map[string]*Session
}

type Manager struct {
	mu         sync.RWMutex
	rooms      map[string]*Room
	maxRooms   int
	maxPerRoom int
}

func NewManager(maxRooms, maxPerRoom int) *Manager {
	return &Manager{
		rooms:      make(map[string]*Room),
		maxRooms:   maxRooms,
		maxPerRoom: maxPerRoom,
	}
}

func (m *Manager) Join(roomID string, s *Session) error {
	if roomID == "" {
		return ErrMissingRoomID
	}
	m.mu.Lock()
	defer m.mu.Unlock()

	room, ok := m.rooms[roomID]
	if !ok {
		if len(m.rooms) >= m.maxRooms {
			return ErrRoomLimit
		}
		room = &Room{ID: roomID, Sessions: make(map[string]*Session)}
		m.rooms[roomID] = room
	}

	if len(room.Sessions) >= m.maxPerRoom {
		return ErrRoomFull
	}
	room.Sessions[s.ID] = s
	return nil
}

func (m *Manager) Leave(roomID, sessionID string) {
	m.mu.Lock()
	defer m.mu.Unlock()

	room, ok := m.rooms[roomID]
	if !ok {
		return
	}
	delete(room.Sessions, sessionID)
	if len(room.Sessions) == 0 {
		delete(m.rooms, roomID)
	}
}

func (m *Manager) Broadcast(roomID, senderID string, payload []byte) {
	m.mu.RLock()
	defer m.mu.RUnlock()
	room, ok := m.rooms[roomID]
	if !ok {
		return
	}
	for id, s := range room.Sessions {
		if id == senderID {
			continue
		}
		select {
		case s.Send <- payload:
		default:
		}
	}
}

func (m *Manager) RoomCount() int {
	m.mu.RLock()
	defer m.mu.RUnlock()
	return len(m.rooms)
}
