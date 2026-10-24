// Command update_test_server imitates the GitHub release API and asset download for the in-game updater.
//
// Start the game with STORM_UPDATE_URL=http://127.0.0.1:8765/latest to point the updater at it.
package main

import (
	"bytes"
	"crypto/sha256"
	"encoding/hex"
	"encoding/json"
	"flag"
	"log"
	"net/http"
	"os"
	"path/filepath"
	"time"
)

type asset struct {
	Name        string `json:"name"`
	DownloadURL string `json:"browser_download_url"`
	Size        int    `json:"size"`
	Digest      string `json:"digest"`
}

type release struct {
	TagName string  `json:"tag_name"`
	Body    string  `json:"body"`
	Assets  []asset `json:"assets"`
}

func main() {
	address := flag.String("addr", "127.0.0.1:8765", "listen address")
	archivePath := flag.String("zip", "", "release archive to serve")
	version := flag.String("version", "", "release tag")
	notesPath := flag.String("notes", "", "release body (markdown)")
	noRange := flag.Bool("norange", false, "ignore Range headers and always send the whole archive")
	flag.Parse()

	archive, err := os.ReadFile(*archivePath)
	if err != nil {
		log.Fatal(err)
	}
	notes, err := os.ReadFile(*notesPath)
	if err != nil {
		log.Fatal(err)
	}
	name := filepath.Base(*archivePath)
	digest := sha256.Sum256(archive)

	http.HandleFunc("/latest", func(w http.ResponseWriter, r *http.Request) {
		log.Printf("API %s", r.UserAgent())
		w.Header().Set("Content-Type", "application/json")
		json.NewEncoder(w).Encode(release{
			TagName: "v" + *version,
			Body:    string(notes),
			Assets: []asset{{
				Name:        name,
				DownloadURL: "http://" + *address + "/download/" + name,
				Size:        len(archive),
				Digest:      "sha256:" + hex.EncodeToString(digest[:]),
			}},
		})
	})
	http.HandleFunc("/download/"+name, func(w http.ResponseWriter, r *http.Request) {
		requested := r.Header.Get("Range")
		if *noRange {
			r.Header.Del("Range")
		}
		log.Printf("DOWNLOAD range=%q", requested)
		http.ServeContent(w, r, name, time.Time{}, bytes.NewReader(archive))
	})
	log.Printf("serving %s (%d bytes) as %s on %s", name, len(archive), *version, *address)
	log.Fatal(http.ListenAndServe(*address, nil))
}
