package rooms

import (
	"crypto/rand"
	"errors"
	"fmt"
	"sort"
	"strings"
	"sync"
	"unicode"
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
	ErrInvalidRoomID          = errors.New("room id must be 1-32 chars [A-Z0-9_-]")
	ErrUnableToAllocateRoomID = errors.New("unable to allocate room id")
)

const (
	MinPlayerID      = 1
	MaxPlayerID      = 4
	MinRoomIDLength  = 1
	MaxRoomIDLength  = 32
	roomCodeAlphabet = "ABCDEFGHJKLMNPQRSTUVWXYZ23456789"
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

func CanonicalizeRoomID(raw string) (string, error) {
	id := strings.ToUpper(strings.TrimSpace(raw))
	if len(id) < MinRoomIDLength || len(id) > MaxRoomIDLength {
		return "", ErrInvalidRoomID
	}
	for _, r := range id {
		if !(unicode.IsDigit(r) || (r >= 'A' && r <= 'Z') || r == '-' || r == '_') {
			return "", ErrInvalidRoomID
		}
	}
	return id, nil
}

func IsValidPlayerID(playerID int) bool {
	return playerID >= MinPlayerID && playerID <= MaxPlayerID
}

func assignNextPlayerID(room *Room) (int, error) {
	for candidate := MinPlayerID; candidate <= room.MaxPlayers; candidate++ {
		inUse := false
		for _, existing := range room.PlayerIDs {
			if existing == candidate {
				inUse = true
				break
			}
		}
		if !inUse {
			return candidate, nil
		}
	}
	return 0, ErrRoomFull
}

func randomRoomID() (string, error) {
	b := make([]byte, 6)
	if _, err := rand.Read(b); err != nil {
		return "", fmt.Errorf("read random: %w", err)
	}
	out := make([]byte, len(b))
	for i := range b {
		out[i] = roomCodeAlphabet[int(b[i])%len(roomCodeAlphabet)]
	}
	return string(out), nil
}

func (m *Manager) Create(hostProvidedCode string, maxPlayers int) (string, error) {
	capacity, err := normalizeMaxPlayers(maxPlayers, m.maxPerRoom)
	if err != nil {
		return "", err
	}

	var canonical string
	if strings.TrimSpace(hostProvidedCode) != "" {
		canonical, err = CanonicalizeRoomID(hostProvidedCode)
		if err != nil {
			return "", err
		}
	}

	m.mu.Lock()
	defer m.mu.Unlock()

	if len(m.rooms) >= m.maxRooms {
		return "", ErrRoomLimit
	}

	if canonical == "" {
		for attempts := 0; attempts < 32; attempts++ {
			generated, genErr := randomRoomID()
			if genErr != nil {
				return "", genErr
			}
			if _, exists := m.rooms[generated]; !exists {
				canonical = generated
				break
			}
		}
		if canonical == "" {
			return "", ErrUnableToAllocateRoomID
		}
	}

	if _, ok := m.rooms[canonical]; ok {
		return "", ErrRoomAlreadyExists
	}
	m.rooms[canonical] = &Room{
		ID:                  canonical,
		MaxPlayers:          capacity,
		Sessions:            make(map[string]*Session),
		PlayerIDs:           make(map[string]int),
		LastSenderSequences: make(map[string]int64),
	}
	return canonical, nil
}

func (m *Manager) Join(roomID string, s *Session) (string, int, int, int, error) {
	if strings.TrimSpace(roomID) == "" {
		return "", 0, 0, 0, ErrMissingRoomID
	}
	canonicalRoomID, err := CanonicalizeRoomID(roomID)
	if err != nil {
		return "", 0, 0, 0, err
	}
	m.mu.Lock()
	defer m.mu.Unlock()

	room, ok := m.rooms[canonicalRoomID]
	if !ok {
		return "", 0, 0, 0, ErrRoomNotFound
	}

	if existing, ok := room.PlayerIDs[s.ID]; ok {
		return room.ID, existing, len(room.Sessions), room.MaxPlayers, nil
	}

	if len(room.Sessions) >= room.MaxPlayers {
		return "", 0, 0, 0, ErrRoomFull
	}
	playerID, assignErr := assignNextPlayerID(room)
	if assignErr != nil {
		return "", 0, 0, 0, assignErr
	}
	room.PlayerIDs[s.ID] = playerID
	room.Sessions[s.ID] = s
	return room.ID, playerID, len(room.Sessions), room.MaxPlayers, nil
}

func (m *Manager) Leave(roomID, sessionID string) {
	m.LeaveWithParticipants(roomID, sessionID)
}

func (m *Manager) LeaveWithParticipants(roomID, sessionID string) (int, []*Session) {
	m.mu.Lock()
	defer m.mu.Unlock()

	room, ok := m.rooms[roomID]
	if !ok {
		return 0, nil
	}
	leftPlayerID := room.PlayerIDs[sessionID]
	delete(room.Sessions, sessionID)
	delete(room.PlayerIDs, sessionID)
	delete(room.LastSenderSequences, sessionID)
	participants := make([]*Session, 0, len(room.Sessions))
	keys := make([]string, 0, len(room.Sessions))
	for id := range room.Sessions {
		keys = append(keys, id)
	}
	sort.Slice(keys, func(i, j int) bool {
		return room.PlayerIDs[keys[i]] < room.PlayerIDs[keys[j]]
	})
	for _, id := range keys {
		participants = append(participants, room.Sessions[id])
	}
	if len(room.Sessions) == 0 {
		delete(m.rooms, roomID)
	}
	return leftPlayerID, participants
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

func _safeTrySend(ch chan []byte, payload []byte) {
	defer func() {
		_ = recover()
	}()
	select {
	case ch <- payload:
	default:
	}
}

func BroadcastOrdered(sessions []*Session, payload []byte) {
	for _, s := range sessions {
		if s == nil || s.Send == nil {
			continue
		}
		_safeTrySend(s.Send, payload)
	}
}

func (m *Manager) RoomCount() int {
	m.mu.RLock()
	defer m.mu.RUnlock()
	return len(m.rooms)
}
