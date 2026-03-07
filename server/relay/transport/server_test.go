package transport

import (
	"encoding/json"
	"fmt"
	"io"
	"log"
	"net"
	"testing"
	"time"

	"mgba-link-relay/config"
	"mgba-link-relay/protocol"
)

func testServer(t *testing.T, secret string) *Server {
	t.Helper()
	cfg := &config.Config{
		Bind:               "127.0.0.1",
		Port:               0,
		Secret:             secret,
		MaxRooms:           8,
		MaxPlayersPerRoom:  4,
		HeartbeatInterval:  500 * time.Millisecond,
		HeartbeatTimeout:   5 * time.Second,
		ClientWriteTimeout: time.Second,
	}
	return NewServer(cfg, log.New(io.Discard, "", 0))
}

func writeIntent(t *testing.T, c net.Conn, payload string) {
	t.Helper()
	if err := protocol.WriteFrame(c, []byte(payload)); err != nil {
		t.Fatalf("write intent: %v", err)
	}
}

func readEvent(t *testing.T, c net.Conn) map[string]any {
	t.Helper()
	_ = c.SetReadDeadline(time.Now().Add(2 * time.Second))
	payload, err := protocol.ReadFrame(c)
	if err != nil {
		t.Fatalf("read event: %v", err)
	}
	var evt map[string]any
	if err := json.Unmarshal(payload, &evt); err != nil {
		t.Fatalf("decode event: %v", err)
	}
	return evt
}

func readEventTimeout(t *testing.T, c net.Conn, timeout time.Duration) map[string]any {
	t.Helper()
	_ = c.SetReadDeadline(time.Now().Add(timeout))
	payload, err := protocol.ReadFrame(c)
	if err != nil {
		t.Fatalf("read event: %v", err)
	}
	var evt map[string]any
	if err := json.Unmarshal(payload, &evt); err != nil {
		t.Fatalf("decode event: %v", err)
	}
	return evt
}

func mustHello(t *testing.T, c net.Conn, token string) {
	t.Helper()
	msg := `{"intent":"hello","protocolVersion":1,"clientSequence":0}`
	if token != "" {
		msg = `{"intent":"hello","protocolVersion":1,"authToken":"` + token + `","clientSequence":0}`
	}
	writeIntent(t, c, msg)
	_ = readEvent(t, c)
}

func TestRejectNonHandshakeIntentBeforeHello(t *testing.T) {
	s := testServer(t, "")
	client, server := net.Pipe()
	defer client.Close()
	go s.handleConn(server)

	writeIntent(t, client, `{"intent":"joinRoom","clientSequence":1,"roomId":"abc"}`)

	errEvt := readEvent(t, client)
	if errEvt["kind"] != "error" || int(errEvt["code"].(float64)) != 400 {
		t.Fatalf("expected error 400, got %#v", errEvt)
	}
	discEvt := readEvent(t, client)
	if discEvt["kind"] != "disconnected" || discEvt["reason"] != "protocolError" {
		t.Fatalf("expected protocol disconnected, got %#v", discEvt)
	}
}

func TestAuthSecretEnforcedAndNoPlayerAssignedOnFailure(t *testing.T) {
	s := testServer(t, "top-secret")
	client, server := net.Pipe()
	defer client.Close()
	go s.handleConn(server)

	writeIntent(t, client, `{"intent":"hello","protocolVersion":1,"authToken":"wrong","clientSequence":0}`)

	errEvt := readEvent(t, client)
	if errEvt["kind"] != "error" || int(errEvt["code"].(float64)) != 401 {
		t.Fatalf("expected error 401, got %#v", errEvt)
	}
	discEvt := readEvent(t, client)
	if discEvt["kind"] != "disconnected" || discEvt["reason"] != "protocolError" {
		t.Fatalf("expected protocol disconnected, got %#v", discEvt)
	}
}

func TestPlayerAssignedOnlyAfterSuccessfulHello(t *testing.T) {
	s := testServer(t, "top-secret")
	client, server := net.Pipe()
	defer client.Close()
	go s.handleConn(server)

	writeIntent(t, client, `{"intent":"hello","protocolVersion":1,"authToken":"top-secret","clientSequence":0}`)
	evt := readEvent(t, client)
	if evt["kind"] != "playerAssigned" {
		t.Fatalf("expected playerAssigned after successful hello, got %#v", evt)
	}
}

func TestPublishLinkEventSequenceValidationAndServerSequenceOrdering(t *testing.T) {
	s := testServer(t, "")
	c1, s1 := net.Pipe()
	defer c1.Close()
	go s.handleConn(s1)
	c2, s2 := net.Pipe()
	defer c2.Close()
	go s.handleConn(s2)

	mustHello(t, c1, "")
	mustHello(t, c2, "")

	writeIntent(t, c1, `{"intent":"createRoom","clientSequence":1,"roomName":"r1","maxPlayers":4}`)
	_ = readEvent(t, c1) // playerAssigned in room
	_ = readEvent(t, c1) // roomJoined

	writeIntent(t, c2, `{"intent":"joinRoom","clientSequence":1,"roomId":"r1"}`)
	_ = readEvent(t, c2) // playerAssigned in room
	_ = readEvent(t, c2) // roomJoined

	writeIntent(t, c1, `{"intent":"publishLinkEvent","clientSequence":2,"event":{"sequence":1,"senderPlayerId":99,"tickMarker":5,"payload":"AQ=="}}`)
	in1a := readEvent(t, c1)
	in1b := readEvent(t, c2)
	if int(in1a["serverSequence"].(float64)) != 1 || int(in1b["serverSequence"].(float64)) != 1 {
		t.Fatalf("expected serverSequence 1 for first publish, got c1=%#v c2=%#v", in1a, in1b)
	}

	writeIntent(t, c1, `{"intent":"publishLinkEvent","clientSequence":3,"event":{"sequence":1,"senderPlayerId":99,"tickMarker":6,"payload":"AQ=="}}`)
	errEvt := readEvent(t, c1)
	if errEvt["kind"] != "error" || int(errEvt["code"].(float64)) != 409 {
		t.Fatalf("expected sequence conflict error 409, got %#v", errEvt)
	}
}

func TestHeartbeatIntentAcknowledged(t *testing.T) {
	s := testServer(t, "")
	client, server := net.Pipe()
	defer client.Close()
	go s.handleConn(server)

	mustHello(t, client, "")
	writeIntent(t, client, `{"intent":"heartbeat","clientSequence":1,"heartbeatCounter":42}`)
	ack := readEvent(t, client)
	if ack["kind"] != "heartbeatAck" || int(ack["heartbeatCounter"].(float64)) != 42 {
		t.Fatalf("expected heartbeatAck with echoed counter, got %#v", ack)
	}
}

func TestHeartbeatTimeoutDropsClientAndNotifiesPeers(t *testing.T) {
	s := testServer(t, "")
	s.cfg.HeartbeatInterval = 10 * time.Millisecond
	s.cfg.HeartbeatTimeout = 30 * time.Millisecond

	c1, s1 := net.Pipe()
	defer c1.Close()
	go s.handleConn(s1)
	c2, s2 := net.Pipe()
	defer c2.Close()
	go s.handleConn(s2)

	mustHello(t, c1, "")
	mustHello(t, c2, "")

	writeIntent(t, c1, `{"intent":"createRoom","clientSequence":1,"roomName":"hb-room","maxPlayers":4}`)
	_ = readEvent(t, c1)
	_ = readEvent(t, c1)
	writeIntent(t, c2, `{"intent":"joinRoom","clientSequence":1,"roomId":"hb-room"}`)
	joinAssigned := readEvent(t, c2)
	if joinAssigned["kind"] != "playerAssigned" {
		t.Fatalf("expected playerAssigned for join, got %#v", joinAssigned)
	}
	leftPlayerID := int(joinAssigned["playerId"].(float64))
	_ = readEvent(t, c2)

	done := make(chan struct{})
	go func() {
		ticker := time.NewTicker(5 * time.Millisecond)
		defer ticker.Stop()
		counter := int64(100)
		for {
			select {
			case <-done:
				return
			case <-ticker.C:
				_ = protocol.WriteFrame(c1, []byte(`{"intent":"heartbeat","clientSequence":2,"heartbeatCounter":`+fmt.Sprint(counter)+`}`))
				counter++
			}
		}
	}()
	defer close(done)

	timeoutErr := readEventTimeout(t, c2, time.Second)
	if timeoutErr["kind"] != "error" || int(timeoutErr["code"].(float64)) != 408 {
		t.Fatalf("expected heartbeat timeout error code 408, got %#v", timeoutErr)
	}
	timeoutDisc := readEvent(t, c2)
	if timeoutDisc["kind"] != "disconnected" || timeoutDisc["reason"] != "networkTimeout" {
		t.Fatalf("expected networkTimeout disconnected, got %#v", timeoutDisc)
	}

	deadline := time.Now().Add(time.Second)
	var peerLeft map[string]any
	for {
		remaining := time.Until(deadline)
		if remaining <= 0 {
			t.Fatalf("timed out waiting for peerLeft for player %d", leftPlayerID)
		}
		evt := readEventTimeout(t, c1, remaining)
		if evt["kind"] == "peerLeft" {
			peerLeft = evt
			break
		}
	}
	if int(peerLeft["playerId"].(float64)) != leftPlayerID || peerLeft["reason"] != "networkTimeout" {
		t.Fatalf("expected peerLeft with networkTimeout reason for player %d, got %#v", leftPlayerID, peerLeft)
	}
}
