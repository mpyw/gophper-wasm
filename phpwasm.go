// Package phpwasm holds PHP compiled to WebAssembly (wasm32-wasip1), for
// github.com/mpyw/gophper to run on wazero.
//
// The binaries are committed gzipped, so that using this package needs only Go.
// scripts/ rebuilds them from php-src, patches/ and compat/.
package phpwasm

import (
	"bytes"
	"compress/gzip"
	"embed"
	"io"
	"io/fs"
	"sync"
)

// ABIVersion numbers the contract between these binaries and their Go host:
// the "gophper" host functions they import, the globals they export, and
// the constants both sides share. ABI.md lists them. It changes whenever any
// of them does, and the host refuses binaries with another ABIVersion.
const ABIVersion = 1

// CLI returns the PHP CLI SAPI.
var CLI = sync.OnceValues(func() ([]byte, error) { return gunzip(cliGzip) })

// CGI returns the PHP CGI SAPI (php-cgi). It runs one request in plain CGI
// mode, or with -b SOCKET serves FastCGI requests on a Unix socket, one at
// a time.
var CGI = sync.OnceValues(func() ([]byte, error) { return gunzip(cgiGzip) })

// SSL holds OpenSSL's configuration (openssl.cnf) and the CA certificates
// (cert.pem, Mozilla's, as curl publishes them). The host mounts it at
// /etc/gophper/ssl, the OPENSSLDIR the binaries are built with.
var SSL fs.FS = mustSub(sslFiles, "ssl")

//go:embed ssl/openssl.cnf ssl/cert.pem
var sslFiles embed.FS

// Licenses holds the license of each component of the binaries and ext/, as
// <component>.txt. Whoever distributes them must pass these on. README.md
// lists which applies to what.
var Licenses fs.FS = mustSub(licenseFiles, "licenses")

//go:embed licenses/*.txt
var licenseFiles embed.FS

//go:embed php.wasm.gz
var cliGzip []byte

//go:embed php-cgi.wasm.gz
var cgiGzip []byte

func mustSub(f fs.FS, dir string) fs.FS {
	sub, err := fs.Sub(f, dir)
	if err != nil {
		panic(err)
	}
	return sub
}

func gunzip(b []byte) ([]byte, error) {
	r, err := gzip.NewReader(bytes.NewReader(b))
	if err != nil {
		return nil, err
	}
	return io.ReadAll(r)
}
