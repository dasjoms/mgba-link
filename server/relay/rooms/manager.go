package rooms

import (
	"errors"
	"sort"
	"sync"
)

var (
	ErrRoomLimit              = errors.New("room limit reached")
	ErrRoomFull               = errors.New("room is full")
	ErrMissingRoomID          = errors.New("room id is required")
	ErrRoomNotFound           = errors.New("room not found")
	ErrRoomAlreadyExists      = errors.New("room already exists")
	ErrInvalidRoomCapacity    = errors.New("room capacity must be between 2 and 4")
	ErrNotRoomMember          = errors.New("sender is not a room member")
	ErrEventSequenceViolation = errors.New("event sequence must be strictly increasing")
)

type Session struct {
	ID     string
	Player string
	Send   chan []byte
}

type Room struct {
	ID                  string
	MaxPlayers          int
	Sessions            map[string]*Session
	PlayerIDs           map[string]int
	LastSenderSequences map[string]int64
	NextPlayerID        int
	ServerSequence      int64
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

func normalizeMaxPlayers(maxPlayers int, fallback int) (int, error) {
	if maxPlayers == 0 {
		maxPlayers = fallback
	}
	if maxPlayers < 2 || maxPlayers > 4 {
		return 0, ErrInvalidRoomCapacity
	}
	return maxPlayers, nil
}

func (m *Manager) Create(roomID string, maxPlayers int) error {
	if roomID == "" {
		return ErrMissingRoomID
	}
	capacity, err := normalizeMaxPlayers(maxPlayers, m.maxPerRoom)
	if err != nil {
		return err
	}

	m.mu.Lock()
	defer m.mu.Unlock()

	if _, ok := m.rooms[roomID]; ok {
		return ErrRoomAlreadyExists
	}
	if len(m.rooms) >= m.maxRooms {
		return ErrRoomLimit
	}
	m.rooms[roomID] = &Room{
		ID:                  roomID,
		MaxPlayers:          capacity,
		Sessions:            make(map[string]*Session),
		PlayerIDs:           make(map[string]int),
		LastSenderSequences: make(map[string]int64),
		NextPlayerID:        1,
	}
	return nil
}

func (m *Manager) Join(roomID string, s *Session) (int, int, error) {
	if roomID == "" {
		return 0, 0, ErrMissingRoomID
	}
	m.mu.Lock()
	defer m.mu.Unlock()

	room, ok := m.rooms[roomID]
	if !ok {
		return 0, 0, ErrRoomNotFound
	}

	if existing, ok := room.PlayerIDs[s.ID]; ok {
		return existing, len(room.Sessions), nil
	}

	if len(room.Sessions) >= room.MaxPlayers {
		return 0, 0, ErrRoomFull
	}
	playerID := room.NextPlayerID
	room.NextPlayerID++
	room.PlayerIDs[s.ID] = playerID
	room.Sessions[s.ID] = s
	return playerID, len(room.Sessions), nil
}

func (m *Manager) Leave(roomID, sessionID string) {
	m.mu.Lock()
	defer m.mu.Unlock()

	room, ok := m.rooms[roomID]
	if !ok {
		return
	}
	delete(room.Sessions, sessionID)
	delete(room.PlayerIDs, sessionID)
	delete(room.LastSenderSequences, sessionID)
	if len(room.Sessions) == 0 {
		delete(m.rooms, roomID)
	}
}

func (m *Manager) ReservePublish(roomID, senderID string, senderSequence int64) (int64, int, []*Session, error) {
	m.mu.Lock()
	defer m.mu.Unlock()

	room, ok := m.rooms[roomID]
	if !ok {
		return 0, 0, nil, ErrRoomNotFound
	}

	senderPlayerID, ok := room.PlayerIDs[senderID]
	if !ok {
		return 0, 0, nil, ErrNotRoomMember
	}

	if last, has := room.LastSenderSequences[senderID]; has && senderSequence <= last {
		return 0, 0, nil, ErrEventSequenceViolation
	}
	room.LastSenderSequences[senderID] = senderSequence
	room.ServerSequence++

	ordered := make([]*Session, 0, len(room.Sessions))
	keys := make([]string, 0, len(room.Sessions))
	for id := range room.Sessions {
		keys = append(keys, id)
	}
	sort.Slice(keys, func(i, j int) bool {
		return room.PlayerIDs[keys[i]] < room.PlayerIDs[keys[j]]
	})
	for _, id := range keys {
		ordered = append(ordered, room.Sessions[id])
	}

	return room.ServerSequence, senderPlayerID, ordered, nil
}

func BroadcastOrdered(sessions []*Session, payload []byte) {
	for _, s := range sessions {
		if s == nil {
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
