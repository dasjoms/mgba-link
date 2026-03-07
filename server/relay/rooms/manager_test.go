package rooms

import "testing"

func TestCreateJoinCapacityAndStablePlayerIDs(t *testing.T) {
	m := NewManager(2, 4)
	if err := m.Create("r1", 2); err != nil {
		t.Fatalf("create room: %v", err)
	}

	s1 := &Session{ID: "a", Send: make(chan []byte, 1)}
	s2 := &Session{ID: "b", Send: make(chan []byte, 1)}
	s3 := &Session{ID: "c", Send: make(chan []byte, 1)}

	p1, _, err := m.Join("r1", s1)
	if err != nil || p1 != 1 {
		t.Fatalf("join s1 failed p=%d err=%v", p1, err)
	}
	p2, _, err := m.Join("r1", s2)
	if err != nil || p2 != 2 {
		t.Fatalf("join s2 failed p=%d err=%v", p2, err)
	}
	if _, _, err := m.Join("r1", s3); err != ErrRoomFull {
		t.Fatalf("expected room full, got %v", err)
	}

	p1b, _, err := m.Join("r1", s1)
	if err != nil || p1b != p1 {
		t.Fatalf("rejoin same session should keep id=%d, got %d err=%v", p1, p1b, err)
	}
}

func TestReservePublishMonotonicAndServerSequence(t *testing.T) {
	m := NewManager(2, 4)
	if err := m.Create("r1", 4); err != nil {
		t.Fatalf("create room: %v", err)
	}
	s1 := &Session{ID: "a", Send: make(chan []byte, 1)}
	s2 := &Session{ID: "b", Send: make(chan []byte, 1)}
	if _, _, err := m.Join("r1", s1); err != nil {
		t.Fatalf("join s1: %v", err)
	}
	if _, _, err := m.Join("r1", s2); err != nil {
		t.Fatalf("join s2: %v", err)
	}

	ss1, sp1, participants1, err := m.ReservePublish("r1", "a", 10)
	if err != nil {
		t.Fatalf("reserve 1: %v", err)
	}
	if ss1 != 1 || sp1 != 1 || len(participants1) != 2 {
		t.Fatalf("unexpected reserve1: ss=%d sp=%d participants=%d", ss1, sp1, len(participants1))
	}

	ss2, _, _, err := m.ReservePublish("r1", "a", 11)
	if err != nil {
		t.Fatalf("reserve 2: %v", err)
	}
	if ss2 != 2 {
		t.Fatalf("expected serverSequence 2, got %d", ss2)
	}

	if _, _, _, err := m.ReservePublish("r1", "a", 11); err != ErrEventSequenceViolation {
		t.Fatalf("expected sequence violation, got %v", err)
	}
	if _, _, _, err := m.ReservePublish("r1", "a", 9); err != ErrEventSequenceViolation {
		t.Fatalf("expected sequence violation for decrease, got %v", err)
	}
}
