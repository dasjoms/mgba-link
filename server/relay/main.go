package main

import (
	"fmt"
	"os"

	"mgba-link-relay/config"
	"mgba-link-relay/logging"
	"mgba-link-relay/transport"
)

func main() {
	cfg, err := config.Load()
	if err != nil {
		_, _ = fmt.Fprintf(os.Stderr, "config error: %v\n", err)
		os.Exit(2)
	}

	logger := logging.New()
	server := transport.NewServer(cfg, logger)
	if err := server.ListenAndServe(); err != nil {
		logger.Fatalf("server stopped: %v", err)
	}
}
