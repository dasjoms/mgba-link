package config

import (
	"flag"
	"fmt"
	"os"
	"strconv"
	"time"
)

// Config stores runtime configuration for the relay service.
type Config struct {
	Bind               string
	Port               int
	Secret             string
	MaxRooms           int
	MaxPlayersPerRoom  int
	HeartbeatInterval  time.Duration
	HeartbeatTimeout   time.Duration
	ClientWriteTimeout time.Duration
	ClientReadBufSize  int
}

func Load() (*Config, error) {
	cfg := &Config{}

	flag.StringVar(&cfg.Bind, "bind", envOrDefault("RELAY_BIND", "0.0.0.0"), "IP/interface to bind")
	flag.IntVar(&cfg.Port, "port", envOrDefaultInt("RELAY_PORT", 41000), "port to listen on")
	flag.StringVar(&cfg.Secret, "secret", envOrDefault("RELAY_SECRET", ""), "shared secret for clients")
	flag.IntVar(&cfg.MaxRooms, "max-rooms", envOrDefaultInt("RELAY_MAX_ROOMS", 1024), "maximum number of active rooms")
	flag.IntVar(&cfg.MaxPlayersPerRoom, "max-players-per-room", envOrDefaultInt("RELAY_MAX_PLAYERS_PER_ROOM", 4), "max players allowed per room")
	flag.DurationVar(&cfg.HeartbeatInterval, "heartbeat-interval", envOrDefaultDuration("RELAY_HEARTBEAT_INTERVAL", 5*time.Second), "server heartbeat interval")
	flag.DurationVar(&cfg.HeartbeatTimeout, "heartbeat-timeout", envOrDefaultDuration("RELAY_HEARTBEAT_TIMEOUT", 20*time.Second), "disconnect clients after no pong within this timeout")
	flag.DurationVar(&cfg.ClientWriteTimeout, "write-timeout", envOrDefaultDuration("RELAY_WRITE_TIMEOUT", 5*time.Second), "write timeout per message")
	flag.IntVar(&cfg.ClientReadBufSize, "read-buffer", envOrDefaultInt("RELAY_READ_BUFFER", 64*1024), "max per-message read buffer in bytes")
	flag.Parse()

	if cfg.Port <= 0 || cfg.Port > 65535 {
		return nil, fmt.Errorf("invalid --port: %d", cfg.Port)
	}
	if cfg.MaxRooms <= 0 {
		return nil, fmt.Errorf("--max-rooms must be > 0")
	}
	if cfg.MaxPlayersPerRoom <= 1 {
		return nil, fmt.Errorf("--max-players-per-room must be > 1")
	}
	if cfg.HeartbeatInterval <= 0 || cfg.HeartbeatTimeout <= 0 {
		return nil, fmt.Errorf("heartbeat values must be > 0")
	}
	if cfg.HeartbeatTimeout < cfg.HeartbeatInterval {
		return nil, fmt.Errorf("--heartbeat-timeout must be >= --heartbeat-interval")
	}

	return cfg, nil
}

func (c *Config) Addr() string {
	return fmt.Sprintf("%s:%d", c.Bind, c.Port)
}

func envOrDefault(key, fallback string) string {
	if v := os.Getenv(key); v != "" {
		return v
	}
	return fallback
}

func envOrDefaultInt(key string, fallback int) int {
	if v := os.Getenv(key); v != "" {
		parsed, err := strconv.Atoi(v)
		if err == nil {
			return parsed
		}
	}
	return fallback
}

func envOrDefaultDuration(key string, fallback time.Duration) time.Duration {
	if v := os.Getenv(key); v != "" {
		parsed, err := time.ParseDuration(v)
		if err == nil {
			return parsed
		}
	}
	return fallback
}
