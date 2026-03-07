package transport

import (
	"encoding/binary"
	"encoding/json"
	"fmt"
	"io"
	"log"
	"net"
	"os"
	"path/filepath"
	"testing"
	"time"

	"mgba-link-relay/config"
	"mgba-link-relay/protocol"
)

type tcpHarnessClient struct {
	t    *testing.T
	conn net.Conn
	name string
}

func (c *tcpHarnessClient) sendJSON(payload string) {
	c.t.Helper()
	if err := protocol.WriteFrame(c.conn, []byte(payload)); err != nil {
		c.t.Fatalf("%s send frame: %v", c.name, err)
	}
}

func (c *tcpHarnessClient) sendBytes(payload []byte) {
	c.t.Helper()
	if err := protocol.WriteFrame(c.conn, payload); err != nil {
		c.t.Fatalf("%s send frame: %v", c.name, err)
	}
}

func (c *tcpHarnessClient) readEvent(timeout time.Duration) map[string]any {
	c.t.Helper()
	_ = c.conn.SetReadDeadline(time.Now().Add(timeout))
	b, err := protocol.ReadFrame(c.conn)
	if err != nil {
		c.t.Fatalf("%s read frame: %v", c.name, err)
	}
	var evt map[string]any
	if err := json.Unmarshal(b, &evt); err != nil {
		c.t.Fatalf("%s unmarshal event: %v", c.name, err)
	}
	return evt
}

func (c *tcpHarnessClient) readUntilKind(timeout time.Duration, kind string) map[string]any {
	c.t.Helper()
	deadline := time.Now().Add(timeout)
	for {
		remaining := time.Until(deadline)
		if remaining <= 0 {
			c.t.Fatalf("%s timed out waiting for kind=%s", c.name, kind)
		}
		evt := c.readEvent(remaining)
		if evt["kind"] == kind {
			return evt
		}
	}
}

func (c *tcpHarnessClient) hello(token string) {
	c.t.Helper()
	msg := `{"intent":"hello","protocolVersion":1,"clientSequence":0}`
	if token != "" {
		msg = fmt.Sprintf(`{"intent":"hello","protocolVersion":1,"clientSequence":0,"authToken":"%s"}`, token)
	}
	c.sendJSON(msg)
}

func startTCPRelayHarness(t *testing.T, secret string, heartbeatInterval, heartbeatTimeout time.Duration) (addr string) {
	t.Helper()
	cfg := &config.Config{
		Bind:               "127.0.0.1",
		Port:               0,
		Secret:             secret,
		MaxRooms:           32,
		MaxPlayersPerRoom:  4,
		HeartbeatInterval:  heartbeatInterval,
		HeartbeatTimeout:   heartbeatTimeout,
		ClientWriteTimeout: time.Second,
	}
	srv := NewServer(cfg, log.New(io.Discard, "", 0))

	ln, err := net.Listen("tcp", cfg.Addr())
	if err != nil {
		t.Fatalf("listen: %v", err)
	}
	t.Cleanup(func() { _ = ln.Close() })

	go func() {
		for {
			conn, err := ln.Accept()
			if err != nil {
				return
			}
			go srv.handleConn(conn)
		}
	}()
	return ln.Addr().String()
}

func dialHarnessClient(t *testing.T, addr, name string) *tcpHarnessClient {
	t.Helper()
	conn, err := net.Dial("tcp", addr)
	if err != nil {
		t.Fatalf("dial %s: %v", name, err)
	}
	client := &tcpHarnessClient{t: t, conn: conn, name: name}
	t.Cleanup(func() { _ = conn.Close() })
	return client
}

func assertKindCode(t *testing.T, evt map[string]any, kind string, code int) {
	t.Helper()
	if evt["kind"] != kind {
		t.Fatalf("expected kind=%s got %#v", kind, evt)
	}
	if code >= 0 && int(evt["code"].(float64)) != code {
		t.Fatalf("expected code=%d got %#v", code, evt)
	}
}

func TestRelayE2EHandshakeRoomRebroadcastAndRejections(t *testing.T) {
	addr := startTCPRelayHarness(t, "top-secret", 200*time.Millisecond, 2*time.Second)

	owner := dialHarnessClient(t, addr, "owner")
	owner.hello("top-secret")
	if evt := owner.readEvent(time.Second); evt["kind"] != "playerAssigned" {
		t.Fatalf("owner expected playerAssigned after hello, got %#v", evt)
	}

	badAuth := dialHarnessClient(t, addr, "bad-auth")
	badAuth.hello("wrong")
	assertKindCode(t, badAuth.readEvent(time.Second), "error", 401)
	if disc := badAuth.readEvent(time.Second); disc["kind"] != "disconnected" || disc["reason"] != "protocolError" {
		t.Fatalf("bad-auth expected protocolError disconnect, got %#v", disc)
	}

	owner.sendJSON(`{"intent":"createRoom","clientSequence":1,"roomName":"e2e-room","maxPlayers":2}`)
	assigned := owner.readEvent(time.Second)
	if assigned["kind"] != "playerAssigned" || int(assigned["playerId"].(float64)) != 1 {
		t.Fatalf("owner expected room playerAssigned=1, got %#v", assigned)
	}
	joined := owner.readEvent(time.Second)
	if joined["kind"] != "roomJoined" || int(joined["memberCount"].(float64)) != 1 {
		t.Fatalf("owner expected roomJoined memberCount=1, got %#v", joined)
	}

	guest := dialHarnessClient(t, addr, "guest")
	guest.hello("top-secret")
	if evt := guest.readEvent(time.Second); evt["kind"] != "playerAssigned" {
		t.Fatalf("guest expected playerAssigned after hello, got %#v", evt)
	}
	guest.sendJSON(`{"intent":"joinRoom","clientSequence":1,"roomId":"e2e-room"}`)
	gAssigned := guest.readEvent(time.Second)
	if gAssigned["kind"] != "playerAssigned" || int(gAssigned["playerId"].(float64)) != 2 {
		t.Fatalf("guest expected room playerAssigned=2, got %#v", gAssigned)
	}
	gJoined := guest.readEvent(time.Second)
	if gJoined["kind"] != "roomJoined" || int(gJoined["memberCount"].(float64)) != 2 {
		t.Fatalf("guest expected roomJoined memberCount=2, got %#v", gJoined)
	}

	fullRoomJoiner := dialHarnessClient(t, addr, "full-room-joiner")
	fullRoomJoiner.hello("top-secret")
	_ = fullRoomJoiner.readEvent(time.Second)
	fullRoomJoiner.sendJSON(`{"intent":"joinRoom","clientSequence":1,"roomId":"e2e-room"}`)
	fullErr := fullRoomJoiner.readEvent(time.Second)
	assertKindCode(t, fullErr, "error", 403)
	if fullDisc := fullRoomJoiner.readEvent(time.Second); fullDisc["kind"] != "disconnected" {
		t.Fatalf("full-room-joiner expected disconnect, got %#v", fullDisc)
	}

	owner.sendJSON(`{"intent":"publishLinkEvent","clientSequence":2,"event":{"sequence":1,"senderPlayerId":999,"tickMarker":10,"payload":"AQ=="}}`)
	ownerInbound1 := owner.readEvent(time.Second)
	guestInbound1 := guest.readEvent(time.Second)
	if ownerInbound1["kind"] != "inboundLinkEvent" || guestInbound1["kind"] != "inboundLinkEvent" {
		t.Fatalf("expected inboundLinkEvent on both clients, owner=%#v guest=%#v", ownerInbound1, guestInbound1)
	}
	if int(ownerInbound1["serverSequence"].(float64)) != 1 || int(guestInbound1["serverSequence"].(float64)) != 1 {
		t.Fatalf("expected serverSequence=1 on first rebroadcast, owner=%#v guest=%#v", ownerInbound1, guestInbound1)
	}

	guest.sendJSON(`{"intent":"publishLinkEvent","clientSequence":2,"event":{"sequence":1,"senderPlayerId":123,"tickMarker":11,"payload":"Ag=="}}`)
	ownerInbound2 := owner.readEvent(time.Second)
	guestInbound2 := guest.readEvent(time.Second)
	if int(ownerInbound2["serverSequence"].(float64)) != 2 || int(guestInbound2["serverSequence"].(float64)) != 2 {
		t.Fatalf("expected serverSequence=2 on second rebroadcast, owner=%#v guest=%#v", ownerInbound2, guestInbound2)
	}

	owner.sendJSON(`{"intent":"publishLinkEvent","clientSequence":3,"event":{"sequence":1,"senderPlayerId":999,"tickMarker":12,"payload":"AQ=="}}`)
	assertKindCode(t, owner.readEvent(time.Second), "error", 409)
	if disc := owner.readEvent(time.Second); disc["kind"] != "disconnected" || disc["reason"] != "protocolError" {
		t.Fatalf("owner expected sequence violation disconnect, got %#v", disc)
	}

	invalidPayload := dialHarnessClient(t, addr, "invalid-payload")
	invalidPayload.hello("top-secret")
	_ = invalidPayload.readEvent(time.Second)
	invalidPayload.sendJSON(`{"intent":"joinRoom","clientSequence":1,"roomId":"e2e-room"}`)
	_ = invalidPayload.readEvent(time.Second)
	_ = invalidPayload.readEvent(time.Second)
	invalidPayload.sendJSON(`{"intent":"publishLinkEvent","clientSequence":2,"event":{"sequence":1,"senderPlayerId":1,"tickMarker":1,"payload":"NOT_BASE64"}}`)
	assertKindCode(t, invalidPayload.readEvent(time.Second), "error", 400)
	if disc := invalidPayload.readEvent(time.Second); disc["kind"] != "disconnected" {
		t.Fatalf("invalid-payload expected disconnect, got %#v", disc)
	}
}

func TestRelayE2EHeartbeatTimeoutCleanup(t *testing.T) {
	addr := startTCPRelayHarness(t, "", 20*time.Millisecond, 80*time.Millisecond)

	alive := dialHarnessClient(t, addr, "alive")
	idle := dialHarnessClient(t, addr, "idle")

	alive.hello("")
	_ = alive.readEvent(time.Second)
	idle.hello("")
	_ = idle.readEvent(time.Second)

	alive.sendJSON(`{"intent":"createRoom","clientSequence":1,"roomName":"hb-e2e","maxPlayers":2}`)
	_ = alive.readEvent(time.Second)
	_ = alive.readEvent(time.Second)
	idle.sendJSON(`{"intent":"joinRoom","clientSequence":1,"roomId":"hb-e2e"}`)
	idleAssigned := idle.readEvent(time.Second)
	leftPlayerID := int(idleAssigned["playerId"].(float64))
	_ = idle.readEvent(time.Second)

	stopHeartbeats := make(chan struct{})
	defer close(stopHeartbeats)
	go func() {
		counter := int64(10)
		t := time.NewTicker(15 * time.Millisecond)
		defer t.Stop()
		for {
			select {
			case <-stopHeartbeats:
				return
			case <-t.C:
				_ = protocol.WriteFrame(alive.conn, []byte(fmt.Sprintf(`{"intent":"heartbeat","clientSequence":2,"heartbeatCounter":%d}`, counter)))
				counter++
			}
		}
	}()

	timeoutErr := idle.readEvent(2 * time.Second)
	assertKindCode(t, timeoutErr, "error", 408)
	if disc := idle.readEvent(time.Second); disc["kind"] != "disconnected" || disc["reason"] != "networkTimeout" {
		t.Fatalf("idle expected networkTimeout disconnect, got %#v", disc)
	}

	peerLeft := alive.readUntilKind(2*time.Second, "peerLeft")
	if peerLeft["kind"] != "peerLeft" || int(peerLeft["playerId"].(float64)) != leftPlayerID || peerLeft["reason"] != "networkTimeout" {
		t.Fatalf("alive expected peerLeft networkTimeout player=%d, got %#v", leftPlayerID, peerLeft)
	}
}

func TestRelayE2ELegacyLinkEventCompatibilityPolicy(t *testing.T) {
	addr := startTCPRelayHarness(t, "", 200*time.Millisecond, 2*time.Second)
	legacy := dialHarnessClient(t, addr, "legacy")

	fixturePath := filepath.Join("testdata", "legacy_link_event.json")
	payload, err := os.ReadFile(fixturePath)
	if err != nil {
		t.Fatalf("read legacy fixture: %v", err)
	}
	legacy.sendBytes(payload)

	errEvt := legacy.readEvent(time.Second)
	assertKindCode(t, errEvt, "error", 400)
	if errEvt["message"] != "missing top-level discriminator: intent" {
		t.Fatalf("expected legacy payload to be rejected with missing intent, got %#v", errEvt)
	}
	if disc := legacy.readEvent(time.Second); disc["kind"] != "disconnected" || disc["reason"] != "protocolError" {
		t.Fatalf("expected protocolError disconnect for legacy payload, got %#v", disc)
	}
}

func TestLegacyFixtureFrameRemainsWithinServerLimit(t *testing.T) {
	payload, err := os.ReadFile(filepath.Join("testdata", "legacy_link_event.json"))
	if err != nil {
		t.Fatalf("read fixture: %v", err)
	}
	if len(payload) > protocol.NetplayMaxFramePayloadSize {
		t.Fatalf("legacy fixture exceeds max frame size: %d", len(payload))
	}

	var header [4]byte
	binary.BigEndian.PutUint32(header[:], uint32(len(payload)))
	if binary.BigEndian.Uint32(header[:]) == 0 {
		t.Fatalf("invalid encoded frame header for fixture")
	}
}
