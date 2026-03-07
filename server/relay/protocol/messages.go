package protocol

import (
	"bufio"
	"encoding/json"
	"fmt"
	"io"
)

type Message struct {
	Type    string          `json:"type"`
	RoomID  string          `json:"roomId,omitempty"`
	Player  string          `json:"player,omitempty"`
	Secret  string          `json:"secret,omitempty"`
	Payload json.RawMessage `json:"payload,omitempty"`
}

type Envelope struct {
	From    string          `json:"from"`
	Type    string          `json:"type"`
	Payload json.RawMessage `json:"payload,omitempty"`
}

func ReadMessage(r *bufio.Reader) (*Message, error) {
	line, err := r.ReadBytes('\n')
	if err != nil {
		if err == io.EOF {
			return nil, io.EOF
		}
		return nil, fmt.Errorf("read line: %w", err)
	}

	var msg Message
	if err := json.Unmarshal(line, &msg); err != nil {
		return nil, fmt.Errorf("decode json: %w", err)
	}
	if msg.Type == "" {
		return nil, fmt.Errorf("missing message type")
	}
	return &msg, nil
}

func WriteJSONLine(w io.Writer, v any) error {
	enc := json.NewEncoder(w)
	if err := enc.Encode(v); err != nil {
		return fmt.Errorf("encode json: %w", err)
	}
	return nil
}
