package librarian

import (
	"bytes"
	"context"
	"fmt"
	"net/http"
	"net/http/httptest"
	"os"
	"path/filepath"
	"strings"
	"testing"

	"github.com/BrandonDedolph/ipod_theme/core/cli/internal/artfetch"
	"github.com/BrandonDedolph/ipod_theme/core/cli/internal/flac"
	"github.com/BrandonDedolph/ipod_theme/core/cli/internal/organizer"
)

// --- low-resolution covers ----------------------------------------------------

// sizedArt is one fake provider that finds every album and serves cover for
// it, whatever it is asked: the low-resolution tests decide the size.
func sizedArt(t *testing.T, cover []byte) (*artfetch.Client, *int) {
	t.Helper()
	fetches := 0
	mux := http.NewServeMux()
	var srv *httptest.Server
	mux.HandleFunc("/search", func(w http.ResponseWriter, r *http.Request) {
		term := r.URL.Query().Get("term")
		for _, a := range []struct{ artist, album string }{
			{"Artist S", "Small"}, {"Artist T", "Tiny"},
		} {
			if strings.Contains(term, a.album) {
				fmt.Fprintf(w, `{"resultCount":1,"results":[{"collectionId":9,`+
					`"artistName":%q,"collectionName":%q,`+
					`"artworkUrl100":"%s/img/%s/100x100bb.jpg"}]}`, a.artist, a.album, srv.URL, a.album)
				return
			}
		}
		w.Write([]byte(`{"resultCount":0,"results":[]}`))
	})
	mux.HandleFunc("/img/", func(w http.ResponseWriter, r *http.Request) {
		fetches++
		w.Write(cover)
	})
	srv = httptest.NewServer(mux)
	t.Cleanup(srv.Close)
	return &artfetch.Client{
		Providers:   []artfetch.Provider{&artfetch.ITunes{BaseURL: srv.URL}},
		UserAgent:   artfetch.UserAgent(),
		StopAtScore: 1.0,
	}, &fetches
}

// lowResFixture is one album with a 300 px cover (and a cover.jpg beside it,
// the old one), one with a 1200 px cover, and one whose picture will not
// decode.
func lowResFixture(t *testing.T) (src string, old []byte) {
	t.Helper()
	src = t.TempDir()
	old = pngSquare(t, 300)
	small := filepath.Join(src, "Small - Artist S")
	writeTrack(t, small, "01 - Artist S - One.flac", map[string]string{
		"album": "Small", "artist": "Artist S", "title": "One", "tracknumber": "1"}, old)
	writeTrack(t, small, "02 - Artist S - Two.flac", map[string]string{
		"album": "Small", "artist": "Artist S", "title": "Two", "tracknumber": "2"}, old)
	if err := os.WriteFile(filepath.Join(small, "cover.jpg"), []byte("the old cover.jpg"), 0o644); err != nil {
		t.Fatal(err)
	}
	writeTrack(t, filepath.Join(src, "Big - Artist B"), "01 - Artist B - One.flac", map[string]string{
		"album": "Big", "artist": "Artist B", "title": "One", "tracknumber": "1"}, pngSquare(t, 1200))
	writeTrack(t, filepath.Join(src, "Broken Art - Artist X"), "01 - Artist X - One.flac", map[string]string{
		"album": "Broken Art", "artist": "Artist X", "title": "One", "tracknumber": "1"}, []byte("not a picture"))
	return src, old
}

func frontCover(t *testing.T, path string) []byte {
	t.Helper()
	m, err := flac.ReadFile(path)
	if err != nil {
		t.Fatal(err)
	}
	for _, p := range m.Pictures {
		if p.Type == flac.PictureTypeFrontCover {
			return p.Data
		}
	}
	return nil
}

func TestInspectFindsLowResArt(t *testing.T) {
	src, _ := lowResFixture(t)
	rep, err := Inspect(context.Background(), src, opts(t))
	if err != nil {
		t.Fatal(err)
	}
	if len(rep.LowResArt) != 1 {
		t.Fatalf("LowResArt = %+v, want the one 300 px album", rep.LowResArt)
	}
	a := rep.LowResArt[0]
	if a.Key != "Small - Artist S" || a.Width != 300 || a.Height != 300 || a.Size() != "300×300" {
		t.Errorf("low-res entry = %+v", a)
	}
	if a.Source != a.FirstFLAC || a.Tracks != 2 {
		t.Errorf("measured in %s (art source %s), %d tracks", a.Source, a.FirstFLAC, a.Tracks)
	}
	if rep.MinArtPx != DefaultMinArtPx {
		t.Errorf("MinArtPx = %d", rep.MinArtPx)
	}
	// A picture that does not decode is no cover as far as the device goes.
	if got := keys(rep); len(got) != 1 || got[0] != "Broken Art - Artist X" {
		t.Errorf("MissingArt = %v, want the undecodable one", got)
	}

	// The line moves with the option, and a negative one turns it off.
	o := opts(t)
	o.MinArtPx = 1500
	if rep, _ := Inspect(context.Background(), src, o); len(rep.LowResArt) != 2 {
		t.Errorf("at 1500 px: %d low-res, want 2", len(rep.LowResArt))
	}
	o.MinArtPx = -1
	if rep, _ := Inspect(context.Background(), src, o); len(rep.LowResArt) != 0 || rep.MinArtPx != 0 {
		t.Errorf("check off: %d low-res, line %d", len(rep.LowResArt), rep.MinArtPx)
	}
}

// TestFixReplacesALowResCoverAndUndoPutsItBack is the whole round trip: the
// candidate is measured, the replacement is reported as 300×300 → 1200×1200,
// every track carries it, and Undo puts the old picture and the old cover.jpg
// back and takes away the sidecars that were not there before.
func TestFixReplacesALowResCoverAndUndoPutsItBack(t *testing.T) {
	src, old := lowResFixture(t)
	big := jpegSquare(t, 1200)
	client, _ := sizedArt(t, big)
	o := opts(t)
	o.Art = client
	small := filepath.Join(src, "Small - Artist S")
	before := tree(t, small)

	rep, err := Inspect(context.Background(), src, o)
	if err != nil {
		t.Fatal(err)
	}
	cands, err := Candidates(context.Background(), rep, client, o)
	if err != nil {
		t.Fatal(err)
	}
	c := cands["Small - Artist S"]
	if len(c) == 0 || c[0].Width != 1200 || c[0].Height != 1200 {
		t.Fatalf("candidates = %+v, want one measured at 1200", c)
	}
	res, err := Fix(context.Background(), rep, Choices{Art: AcceptAbove(cands, DefaultMinScore)}, o)
	if err != nil {
		t.Fatalf("Fix: %v", err)
	}
	if len(res.Replaced) != 1 || res.Replaced[0].String() != "replaced 300×300 → 1200×1200" {
		t.Fatalf("Replaced = %+v", res.Replaced)
	}
	if res.Journal == "" || !strings.HasSuffix(res.Journal, "-fix.json") {
		t.Fatalf("journal = %q, want a librarian journal", res.Journal)
	}
	for _, f := range []string{"01 - Artist S - One.flac", "02 - Artist S - Two.flac"} {
		if !bytes.Equal(frontCover(t, filepath.Join(small, f)), big) {
			t.Errorf("%s does not carry the new cover", f)
		}
	}
	if b, _ := os.ReadFile(filepath.Join(small, "cover.jpg")); !bytes.Equal(b, big) {
		t.Error("cover.jpg is not the new cover")
	}
	// organizer's own listing does not mistake this journal for one of its.
	if js, err := organizer.ListJournals(o.JournalDir); err != nil || len(js) != 0 {
		t.Errorf("organizer lists %d journal(s) (%v)", len(js), err)
	}

	undo, err := Undo(context.Background(), res.Journal, o)
	if err != nil {
		t.Fatalf("Undo: %v", err)
	}
	if len(undo.Skipped) != 0 {
		t.Errorf("Undo skipped %+v", undo.Skipped)
	}
	for _, f := range []string{"01 - Artist S - One.flac", "02 - Artist S - Two.flac"} {
		if !bytes.Equal(frontCover(t, filepath.Join(small, f)), old) {
			t.Errorf("%s did not get the old cover back", f)
		}
	}
	after := tree(t, small)
	for name, sum := range before {
		if strings.HasSuffix(name, ".flac") {
			continue // the picture block is the same; the padding need not be
		}
		if after[name] != sum {
			t.Errorf("%s did not come back as it was", name)
		}
	}
	for name := range after {
		if _, ok := before[name]; !ok {
			t.Errorf("%s is left over after the undo", name)
		}
	}
}

// A cover no larger than the one the album has is never taken: Candidates
// drops it, and a decision that names it anyway fails in Fix and leaves the
// files alone.
func TestALowResCoverIsOnlyReplacedByALargerOne(t *testing.T) {
	src, old := lowResFixture(t)
	client, _ := sizedArt(t, jpegSquare(t, 300))
	o := opts(t)
	o.Art = client
	rep, err := Inspect(context.Background(), src, o)
	if err != nil {
		t.Fatal(err)
	}
	cands, err := Candidates(context.Background(), rep, client, o)
	if err != nil {
		t.Fatal(err)
	}
	if c := cands["Small - Artist S"]; len(c) != 0 {
		t.Errorf("an equal-sized cover was offered: %+v", c)
	}
	forced := artfetch.Candidate{Provider: "itunes", Artist: "Artist S", Album: "Small",
		FullURL: client.Providers[0].(*artfetch.ITunes).BaseURL + "/img/Small/1200x1200bb.jpg"}
	res, err := Fix(context.Background(), rep, Choices{Art: map[AlbumKey]Decision{
		"Small - Artist S": Accept(forced)}}, o)
	if err != nil {
		t.Fatal(err)
	}
	if len(res.Failures) != 1 || !strings.Contains(res.Failures[0].Reason, "no larger") {
		t.Errorf("failures = %+v", res.Failures)
	}
	if len(res.Replaced) != 0 || res.Journal != "" {
		t.Errorf("replaced %+v, journal %q", res.Replaced, res.Journal)
	}
	if !bytes.Equal(frontCover(t, filepath.Join(src, "Small - Artist S", "01 - Artist S - One.flac")), old) {
		t.Error("the file was written")
	}
}

// --- duplicates ------------------------------------------------------------------

// durTrack is writeTrack with a length: 8 kHz keeps a four-minute fixture to
// a few kilobytes of silent frames.
func durTrack(t *testing.T, dir, name, album, artist, title string, n, seconds int) {
	t.Helper()
	if err := os.MkdirAll(dir, 0o755); err != nil {
		t.Fatal(err)
	}
	raw := flac.BuildFile(
		flac.StreamInfo{SampleRate: 8000, Channels: 1, BitsPerSample: 16, TotalSamples: uint64(seconds) * 8000},
		map[string]string{"album": album, "artist": artist, "title": title, "tracknumber": fmt.Sprint(n)},
		[]flac.Picture{{Type: flac.PictureTypeFrontCover, MIME: "image/png", Data: smallPNG(t)}})
	if err := os.WriteFile(filepath.Join(dir, name), raw, 0o644); err != nil {
		t.Fatal(err)
	}
}

var swagSongs = []struct {
	title string
	secs  int
}{{"ALL I CAN TAKE", 201}, {"DAISIES", 176}, {"YUKON", 188}, {"GO BABY", 214}, {"POPPIN’ MY S___", 160}}

// swagFixture is the owner's case: SWAG on its own, and SWAG II with the same
// five songs as its Disc 2 (one length a second off, one title with a
// straight quote where the other has a curly one, one artist in other case).
func swagFixture(t *testing.T) string {
	t.Helper()
	src := t.TempDir()
	swag := filepath.Join(src, "SWAG - Justin Bieber")
	for i, s := range swagSongs {
		durTrack(t, swag, fmt.Sprintf("%02d - Justin Bieber - %d.flac", i+1, i), "SWAG", "Justin Bieber", s.title, i+1, s.secs)
	}
	two := filepath.Join(src, "SWAG II - Justin Bieber")
	for i, title := range []string{"SPEED DEMON", "BETTER MAN", "I DO"} {
		durTrack(t, filepath.Join(two, "Disc 1"), fmt.Sprintf("%02d - Justin Bieber - %d.flac", i+1, i),
			"SWAG II", "Justin Bieber", title, i+1, 150+i)
	}
	for i, s := range swagSongs {
		title, secs, artist := s.title, s.secs, "Justin Bieber"
		switch i {
		case 1:
			secs++
		case 2:
			artist = "JUSTIN BIEBER"
		case 4:
			title = "POPPIN' MY S___"
		}
		durTrack(t, filepath.Join(two, "Disc 2"), fmt.Sprintf("%02d - Justin Bieber - %d.flac", i+1, i),
			"SWAG II", artist, title, i+1, secs)
	}
	return src
}

func TestDuplicatesFindTheSWAGCase(t *testing.T) {
	src := swagFixture(t)
	rep, err := Inspect(context.Background(), src, opts(t))
	if err != nil {
		t.Fatal(err)
	}
	if len(rep.Duplicates) != 1 {
		t.Fatalf("Duplicates = %+v, want one group", rep.Duplicates)
	}
	g := rep.Duplicates[0]
	if g.A.Key != "SWAG - Justin Bieber" || g.B.Key != "SWAG II - Justin Bieber" {
		t.Errorf("A = %s, B = %s", g.A.Key, g.B.Key)
	}
	if g.Matching != 5 || g.Same || g.A.Tracks != 5 || g.B.Tracks != 8 {
		t.Errorf("matching %d, same %v, tracks %d/%d", g.Matching, g.Same, g.A.Tracks, g.B.Tracks)
	}
	if g.A.Extra != 0 || g.B.Extra != 3 || g.B.Disc != "Disc 2" {
		t.Errorf("extra %d/%d, disc %q", g.A.Extra, g.B.Extra, g.B.Disc)
	}
	if want := "SWAG's 5 tracks are all in SWAG II (as Disc 2)"; g.Why != want {
		t.Errorf("Why = %q, want %q", g.Why, want)
	}
	if g.A.Bytes <= 0 || g.B.Bytes <= g.A.Bytes {
		t.Errorf("sizes %d / %d", g.A.Bytes, g.B.Bytes)
	}
	if rep.Empty() {
		t.Error("a report with a duplicate says it is empty")
	}
}

// Two folders holding the same record are ONE group, marked Same.
func TestDuplicatesTheSameAlbumTwice(t *testing.T) {
	src := t.TempDir()
	for _, dir := range []string{"Hounds - Kate Bush", "Downloads/Hounds of Love - Kate Bush"} {
		for i, s := range swagSongs[:4] {
			durTrack(t, filepath.Join(src, filepath.FromSlash(dir)), fmt.Sprintf("%02d.flac", i+1),
				"Hounds", "Kate Bush", s.title, i+1, s.secs)
		}
	}
	rep, err := Inspect(context.Background(), src, opts(t))
	if err != nil {
		t.Fatal(err)
	}
	if len(rep.Duplicates) != 1 || !rep.Duplicates[0].Same {
		t.Fatalf("Duplicates = %+v", rep.Duplicates)
	}
	if !strings.Contains(rep.Duplicates[0].Why, "hold the same 4 tracks") {
		t.Errorf("Why = %q", rep.Duplicates[0].Why)
	}
}

// Four of five songs (80 %) is two albums that share some songs, not a
// duplicate; and a length three seconds off is another recording.
func TestDuplicatesNearMissesAreNotReported(t *testing.T) {
	src := t.TempDir()
	for i, s := range swagSongs {
		durTrack(t, filepath.Join(src, "One - Artist"), fmt.Sprintf("%02d.flac", i+1), "One", "Artist", s.title, i+1, s.secs)
	}
	for i, s := range swagSongs[:4] {
		durTrack(t, filepath.Join(src, "Two - Artist"), fmt.Sprintf("%02d.flac", i+1), "Two", "Artist", s.title, i+1, s.secs)
	}
	durTrack(t, filepath.Join(src, "Two - Artist"), "05.flac", "Two", "Artist", "Something Else", 5, 99)
	for i, s := range swagSongs {
		durTrack(t, filepath.Join(src, "Live - Artist"), fmt.Sprintf("%02d.flac", i+1), "Live", "Artist", s.title, i+1, s.secs+3)
	}
	rep, err := Inspect(context.Background(), src, opts(t))
	if err != nil {
		t.Fatal(err)
	}
	if len(rep.Duplicates) != 0 {
		t.Errorf("near misses were reported: %+v", rep.Duplicates)
	}
}

// Keeping one moves the other folder to the trash beside the music folder,
// journaled; Undo brings it back.
func TestRemoveDuplicateTrashesAndUndoRestores(t *testing.T) {
	src := swagFixture(t)
	o := opts(t)
	o.TrashDir = filepath.Join(t.TempDir(), TrashName)
	rep, err := Inspect(context.Background(), src, o)
	if err != nil {
		t.Fatal(err)
	}
	swag := filepath.Join(src, "SWAG - Justin Bieber")
	before := tree(t, swag)

	if _, err := RemoveDuplicate(context.Background(), rep, "SWAG - Justin Bieber", "Nope - Nobody", o); err == nil {
		t.Error("a pair that is not in the report was acted on")
	}
	dry := o
	dry.DryRun = true
	if res, err := RemoveDuplicate(context.Background(), rep, "SWAG II - Justin Bieber", "SWAG - Justin Bieber", dry); err != nil || res.Trashed != 1 {
		t.Fatalf("dry run: %+v, %v", res, err)
	}
	if _, err := os.Stat(swag); err != nil {
		t.Fatal("the dry run moved the folder")
	}

	res, err := RemoveDuplicate(context.Background(), rep, "SWAG II - Justin Bieber", "SWAG - Justin Bieber", o)
	if err != nil {
		t.Fatalf("RemoveDuplicate: %v", err)
	}
	if res.Trashed != 1 || res.Journal == "" {
		t.Fatalf("result %+v", res)
	}
	if _, err := os.Stat(swag); !os.IsNotExist(err) {
		t.Error("SWAG is still in the music folder")
	}
	rep2, err := Inspect(context.Background(), src, o)
	if err != nil {
		t.Fatal(err)
	}
	if len(rep2.Duplicates) != 0 {
		t.Errorf("after the keep: %+v", rep2.Duplicates)
	}

	undo, err := Undo(context.Background(), res.Journal, o)
	if err != nil || undo.Undone != 1 || len(undo.Skipped) != 0 {
		t.Fatalf("Undo: %+v, %v", undo, err)
	}
	if !sameTree(before, tree(t, swag)) {
		t.Error("SWAG did not come back as it was")
	}
	if ents, _ := os.ReadDir(o.TrashDir); len(ents) != 0 {
		t.Errorf("the trash still holds %d folder(s)", len(ents))
	}
}

// A drive's root has no folder beside it for the trash.
func TestTrashRefusesADriveRoot(t *testing.T) {
	root := string(filepath.Separator)
	if _, err := (Options{}).trashDir(root); err == nil {
		t.Error("a trash beside the root of a drive was accepted")
	}
	if d, err := (Options{}).trashDir(filepath.Join(root, "Music", "MC")); err != nil ||
		filepath.Dir(d) != filepath.Join(root, "Music", TrashName) {
		t.Errorf("trash for /Music/MC = %q, %v", d, err)
	}
}

// --- the folder's own cover file --------------------------------------------------

// An album with no embedded cover takes the folder's cover.png, with no
// client and no network at all.
func TestMissingArtTakesTheFoldersCoverFile(t *testing.T) {
	src := t.TempDir()
	dir := filepath.Join(src, "Canon - Artist A")
	writeTrack(t, dir, "01 - Artist A - One.flac", map[string]string{
		"album": "Canon", "artist": "Artist A", "title": "One", "tracknumber": "1"}, nil)
	file := pngSquare(t, 700)
	if err := os.WriteFile(filepath.Join(dir, "Cover.PNG"), file, 0o644); err != nil {
		t.Fatal(err)
	}
	o := opts(t) // no Art client
	rep, err := Inspect(context.Background(), src, o)
	if err != nil {
		t.Fatal(err)
	}
	if len(rep.MissingArt) != 1 || rep.MissingArt[0].CoverWidth != 700 {
		t.Fatalf("MissingArt = %+v", rep.MissingArt)
	}
	cands, err := Candidates(context.Background(), rep, nil, o)
	if err != nil {
		t.Fatalf("Candidates with no client: %v", err)
	}
	c := cands["Canon - Artist A"]
	if len(c) != 1 || c[0].Provider != ProviderFile || c[0].Score < DefaultMinScore {
		t.Fatalf("candidates = %+v", c)
	}
	res, err := Fix(context.Background(), rep, Choices{Art: AcceptAbove(cands, DefaultMinScore)}, o)
	if err != nil || res.ArtWritten != 1 {
		t.Fatalf("Fix: %+v, %v", res, err)
	}
	if !bytes.Equal(frontCover(t, filepath.Join(dir, "01 - Artist A - One.flac")), file) {
		t.Error("the folder's cover is not the embedded one")
	}
}

// A small embedded cover with a larger cover.jpg beside it is replaced from
// the file — no lookup — and the line says where it came from.
func TestALowResCoverIsReplacedFromALargerFolderFile(t *testing.T) {
	src, old := lowResFixture(t)
	small := filepath.Join(src, "Small - Artist S")
	file := jpegSquare(t, 1400)
	if err := os.WriteFile(filepath.Join(small, "cover.jpg"), file, 0o644); err != nil {
		t.Fatal(err)
	}
	client, fetches := sizedArt(t, jpegSquare(t, 1200))
	o := opts(t)
	o.Art = client
	rep, err := Inspect(context.Background(), src, o)
	if err != nil {
		t.Fatal(err)
	}
	cands, err := Candidates(context.Background(), rep, client, o)
	if err != nil {
		t.Fatal(err)
	}
	if c := cands["Small - Artist S"]; len(c) != 1 || c[0].Provider != ProviderFile || c[0].Width != 1400 {
		t.Fatalf("candidates = %+v", c)
	}
	if *fetches != 0 {
		t.Errorf("%d download(s) for an album with a larger file beside it", *fetches)
	}
	res, err := Fix(context.Background(), rep, Choices{Art: AcceptAbove(cands, DefaultMinScore)}, o)
	if err != nil || len(res.Replaced) != 1 {
		t.Fatalf("Fix: %+v, %v", res, err)
	}
	if got := res.Replaced[0].String(); got != "replaced embedded 300×300 → cover.jpg 1400×1400" {
		t.Errorf("line = %q", got)
	}
	if !bytes.Equal(frontCover(t, filepath.Join(small, "01 - Artist S - One.flac")), file) {
		t.Error("the file's cover was not embedded")
	}
	if _, err := Undo(context.Background(), res.Journal, o); err != nil {
		t.Fatal(err)
	}
	if !bytes.Equal(frontCover(t, filepath.Join(small, "01 - Artist S - One.flac")), old) {
		t.Error("undo did not put the 300 px cover back")
	}
	if b, _ := os.ReadFile(filepath.Join(small, "cover.jpg")); !bytes.Equal(b, file) {
		t.Error("undo changed the cover.jpg the new cover came from")
	}
}

// A folder file no larger than the embedded cover is not a source; the
// lookup runs as before.
func TestASmallerFolderFileDoesNotStopTheLookup(t *testing.T) {
	src, _ := lowResFixture(t)
	if err := os.WriteFile(filepath.Join(src, "Small - Artist S", "folder.jpg"), jpegSquare(t, 200), 0o644); err != nil {
		t.Fatal(err)
	}
	client, fetches := sizedArt(t, jpegSquare(t, 1200))
	o := opts(t)
	rep, err := Inspect(context.Background(), src, o)
	if err != nil {
		t.Fatal(err)
	}
	if len(LocalCandidates(rep)) != 0 {
		t.Errorf("a 200 px file was offered for a 300 px cover: %+v", LocalCandidates(rep))
	}
	cands, err := Candidates(context.Background(), rep, client, o)
	if err != nil {
		t.Fatal(err)
	}
	if c := cands["Small - Artist S"]; len(c) == 0 || c[0].Provider == ProviderFile || *fetches == 0 {
		t.Errorf("candidates = %+v after %d fetches", c, *fetches)
	}
}
