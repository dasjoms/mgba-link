package logging

import (
	"log"
	"os"
)

func New() *log.Logger {
	logger := log.New(os.Stdout, "relay ", log.LstdFlags|log.Lmicroseconds|log.LUTC)
	return logger
}
