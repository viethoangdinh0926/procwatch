// Minimal Go workload for the demo stack.
//
// This binary is built with CGO_ENABLED=0 (static, no PT_INTERP), so
// LD_PRELOAD is a silent no-op. Process metrics come from procwatch-wrap,
// which forks this process as a child and POSTs /v1/procmetrics under
// PROCWATCH_LABEL. No in-process inject thread is possible without ptrace.
//
// The parent also re-execs itself as --worker children so wrap's process-tree
// sampler records multiple pids under inventory_procs.

package main

import (
	"encoding/json"
	"fmt"
	"log"
	"math/rand"
	"net/http"
	"os"
	"os/exec"
	"strconv"
	"time"
)

func listenPort() string {
	if p := os.Getenv("PORT"); p != "" {
		return p
	}
	return "8080"
}

func workerCount() int {
	raw := os.Getenv("PROCWATCH_DEMO_WORKERS")
	if raw == "" {
		return 2
	}
	n, err := strconv.Atoi(raw)
	if err != nil || n < 0 {
		return 2
	}
	return n
}

type stockResponse struct {
	SKU       string `json:"sku"`
	Available int    `json:"available"`
}

func runWorker(id string) {
	log.Printf("go worker-%s pid=%d", id, os.Getpid())
	retained := make([][]byte, 0, 8)
	for {
		ballast := make([]byte, 256<<10)
		sum := 0
		deadline := time.Now().Add(50 * time.Millisecond)
		for time.Now().Before(deadline) {
			sum += rand.Intn(1000)
		}
		_ = sum
		if len(retained) < 8 {
			retained = append(retained, ballast)
		}
		time.Sleep(500 * time.Millisecond)
	}
}

func startWorkers(count int) []*exec.Cmd {
	self, err := os.Executable()
	if err != nil {
		log.Printf("cannot resolve executable for workers: %v", err)
		return nil
	}
	children := make([]*exec.Cmd, 0, count)
	for i := 0; i < count; i++ {
		cmd := exec.Command(self, "--worker", strconv.Itoa(i))
		cmd.Stdout = os.Stdout
		cmd.Stderr = os.Stderr
		cmd.Env = os.Environ()
		if err := cmd.Start(); err != nil {
			log.Printf("failed to spawn go worker-%d: %v", i, err)
			continue
		}
		log.Printf("spawned go worker-%d pid=%d", i, cmd.Process.Pid)
		children = append(children, cmd)
	}
	return children
}

func main() {
	if len(os.Args) > 1 && os.Args[1] == "--worker" {
		id := "0"
		if len(os.Args) > 2 {
			id = os.Args[2]
		}
		runWorker(id)
		return
	}

	service := os.Getenv("PROCWATCH_SERVICE")
	if service == "" {
		service = "inventory"
	}

	children := startWorkers(workerCount())
	defer func() {
		for _, c := range children {
			if c.Process != nil {
				_ = c.Process.Kill()
			}
		}
	}()

	http.HandleFunc("/stock", func(w http.ResponseWriter, r *http.Request) {
		time.Sleep(time.Duration(20+rand.Intn(60)) * time.Millisecond)
		w.Header().Set("Content-Type", "application/json")
		json.NewEncoder(w).Encode(stockResponse{
			SKU:       "SKU-1234",
			Available: rand.Intn(100),
		})
	})
	http.HandleFunc("/health", func(w http.ResponseWriter, r *http.Request) {
		fmt.Fprint(w, "ok")
	})

	// Varying CPU and heap so the collected metrics are not a flat line.
	go func() {
		for {
			ballast := make([]byte, 8<<20)
			for i := range ballast {
				ballast[i] = byte(i)
			}
			sum := 0
			deadline := time.Now().Add(300 * time.Millisecond)
			for time.Now().Before(deadline) {
				sum += rand.Intn(1000)
			}
			_ = sum
			_ = ballast
			time.Sleep(4 * time.Second)
		}
	}()

	port := listenPort()
	log.Printf("%s listening on %s (metrics only, no trace injection)", service, port)
	log.Fatal(http.ListenAndServe(":"+port, nil))
}
