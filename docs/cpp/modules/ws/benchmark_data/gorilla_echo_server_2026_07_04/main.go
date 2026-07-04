package main

import (
	"flag"
	"log"
	"net/http"
	"strconv"

	"github.com/gorilla/websocket"
)

var upgrader = websocket.Upgrader{
	EnableCompression: false,
	CheckOrigin: func(*http.Request) bool {
		return true
	},
}

func echoHandler(w http.ResponseWriter, r *http.Request) {
	conn, err := upgrader.Upgrade(w, r, nil)
	if err != nil {
		return
	}
	defer conn.Close()

	if err := conn.WriteMessage(websocket.TextMessage, []byte("Welcome to WebSocket Benchmark Server!")); err != nil {
		return
	}

	for {
		messageType, payload, err := conn.ReadMessage()
		if err != nil {
			return
		}
		if messageType == websocket.CloseMessage {
			return
		}
		if err := conn.WriteMessage(messageType, payload); err != nil {
			return
		}
	}
}

func main() {
	port := flag.Int("port", 18081, "listen port")
	path := flag.String("path", "/ws", "websocket path")
	flag.Parse()

	mux := http.NewServeMux()
	mux.HandleFunc(*path, echoHandler)

	addr := "127.0.0.1:" + strconv.Itoa(*port)
	log.Printf("gorilla websocket echo server listening on ws://%s%s", addr, *path)
	if err := http.ListenAndServe(addr, mux); err != nil {
		log.Fatal(err)
	}
}
