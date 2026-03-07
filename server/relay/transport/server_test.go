package transport

import (
	"encoding/json"
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
