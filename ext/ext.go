// Package ext holds PHP extensions built as wasm side modules, for php.wasm
// to load at runtime with extension= or dl().
//
// gd, intl, sodium and zip carry their libraries. They get libc, zlib,
// OpenSSL and the C++ runtime from php.wasm, which exports them. dl_test is
// php-src's extension for testing dl(), so that hosts can test their
// dynamic linker.
//
// An extension may need files beside it in the extension directory, such
// as intl's ICU data. Files lists them.
package ext

import (
	"bytes"
	"compress/gzip"
	"embed"
	"io"
	"io/fs"
	"strings"
)

//go:embed *.gz
var files embed.FS

// Names returns the extensions in the package.
func Names() []string {
	entries, _ := fs.ReadDir(files, ".")
	var names []string
	for _, e := range entries {
		if name, ok := strings.CutSuffix(e.Name(), ".so.gz"); ok {
			names = append(names, name)
		}
	}
	return names
}

// Open returns an extension's .so, decompressed.
func Open(name string) ([]byte, error) {
	return gunzip(name + ".so.gz")
}

// Files returns the names of the files an extension needs beside its .so
// in the extension directory.
func Files(name string) []string {
	entries, _ := fs.ReadDir(files, ".")
	var out []string
	for _, e := range entries {
		rest, ok := strings.CutPrefix(e.Name(), name+"-")
		if ok && strings.HasSuffix(rest, ".gz") {
			out = append(out, strings.TrimSuffix(rest, ".gz"))
		}
	}
	return out
}

// OpenFile returns one of an extension's files, decompressed.
func OpenFile(name, file string) ([]byte, error) {
	return gunzip(name + "-" + file + ".gz")
}

func gunzip(path string) ([]byte, error) {
	b, err := files.ReadFile(path)
	if err != nil {
		return nil, err
	}
	r, err := gzip.NewReader(bytes.NewReader(b))
	if err != nil {
		return nil, err
	}
	return io.ReadAll(r)
}
