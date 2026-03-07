package rooms

import "testing"

func TestCreateJoinCapacityAndStablePlayerIDs(t *testing.T) {
	m := NewManager(2, 4)
	roomID, err := m.Create("r1", 2)
	if err != nil {
		t.Fatalf("create room: %v", err)
	}
	if roomID != "R1" {
		t.Fatalf("expected canonical room ID R1, got %q", roomID)
	}

	s1 := &Session{ID: "a", Send: make(chan []byte, 1)}
	s2 := &Session{ID: "b", Send: make(chan []byte, 1)}
	s3 := &Session{ID: "c", Send: make(chan []byte, 1)}

	joinedRoomID, p1, _, _, err := m.Join("r1", s1)
	if err != nil || p1 != 1 || joinedRoomID != "R1" {
		t.Fatalf("join s1 failed room=%q p=%d err=%v", joinedRoomID, p1, err)
	}
	_, p2, _, _, err := m.Join("R1", s2)
	if err != nil || p2 != 2 {
		t.Fatalf("join s2 failed p=%d err=%v", p2, err)
	}
	if _, _, _, _, err := m.Join("R1", s3); err != ErrRoomFull {
		t.Fatalf("expected room full, got %v", err)
	}

	_, p1b, _, _, err := m.Join("R1", s1)
	if err != nil || p1b != p1 {
		t.Fatalf("rejoin same session should keep id=%d, got %d err=%v", p1, p1b, err)
	}
}

func TestReservePublishMonotonicAndServerSequence(t *testing.T) {
	m := NewManager(2, 4)
	if _, err := m.Create("r1", 4); err != nil {
		t.Fatalf("create room: %v", err)
	}
	s1 := &Session{ID: "a", Send: make(chan []byte, 1)}
	s2 := &Session{ID: "b", Send: make(chan []byte, 1)}
	if _, _, _, _, err := m.Join("r1", s1); err != nil {
		t.Fatalf("join s1: %v", err)
	}
	if _, _, _, _, err := m.Join("r1", s2); err != nil {
		t.Fatalf("join s2: %v", err)
	}

	ss1, sp1, participants1, err := m.ReservePublish("R1", "a", 10)
	if err != nil {
		t.Fatalf("reserve 1: %v", err)
	}
	if ss1 != 1 || sp1 != 1 || len(participants1) != 2 {
		t.Fatalf("unexpected reserve1: ss=%d sp=%d participants=%d", ss1, sp1, len(participants1))
	}

	ss2, _, _, err := m.ReservePublish("R1", "a", 11)
	if err != nil {
		t.Fatalf("reserve 2: %v", err)
	}
	if ss2 != 2 {
		t.Fatalf("expected serverSequence 2, got %d", ss2)
	}

	if _, _, _, err := m.ReservePublish("R1", "a", 11); err != ErrEventSequenceViolation {
		t.Fatalf("expected sequence violation, got %v", err)
	}
	if _, _, _, err := m.ReservePublish("R1", "a", 9); err != ErrEventSequenceViolation {
		t.Fatalf("expected sequence violation for decrease, got %v", err)
	}
}

func TestGeneratedRoomIDAndPlayerIDReuse(t *testing.T) {
	m := NewManager(2, 4)
	roomID, err := m.Create("", 2)
	if err != nil {
		t.Fatalf("create generated room: %v", err)
	}
	if len(roomID) != 6 {
		t.Fatalf("expected generated room ID length 6, got %q", roomID)
	}

	s1 := &Session{ID: "a", Send: make(chan []byte, 1)}
	s2 := &Session{ID: "b", Send: make(chan []byte, 1)}
	s3 := &Session{ID: "c", Send: make(chan []byte, 1)}
	_, p1, _, _, _ := m.Join(roomID, s1)
	_, p2, _, _, _ := m.Join(roomID, s2)
	if p1 != 1 || p2 != 2 {
		t.Fatalf("unexpected player IDs: %d %d", p1, p2)
	}
	m.Leave(roomID, s1.ID)
	_, p3, _, _, err := m.Join(roomID, s3)
	if err != nil {
		t.Fatalf("join s3: %v", err)
	}
	if p3 != 1 {
		t.Fatalf("expected freed player id reuse to 1, got %d", p3)
	}
}
