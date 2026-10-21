package phpwasm_test

import (
	"bytes"
	"context"
	"crypto/sha256"
	"encoding/hex"
	"strings"
	"testing"

	"github.com/tetratelabs/wazero"
	"github.com/tetratelabs/wazero/api"
	"github.com/tetratelabs/wazero/experimental"
	"github.com/tetratelabs/wazero/imports/wasi_snapshot_preview1"
	"github.com/tetratelabs/wazero/sys"

	phpwasm "github.com/mpyw/gophper-wasm"
	"github.com/mpyw/gophper-wasm/ext"
)

// errnoNotSupported is WASI's ENOTSUP. Every stubbed host function returns
// it, or nothing.
const errnoNotSupported = 58

// TestCLI runs the CLI with every "gophper" host function stubbed out. That
// is enough for code that touches no socket, timer or extension.
func TestCLI(t *testing.T) {
	ctx := context.Background()
	r := wazero.NewRuntimeWithConfig(ctx, wazero.NewRuntimeConfig().
		WithCoreFeatures(api.CoreFeaturesV2|experimental.CoreFeaturesExceptionHandling|experimental.CoreFeaturesExtendedConst))
	defer r.Close(ctx)
	wasi_snapshot_preview1.MustInstantiate(ctx, r)

	wasm, err := phpwasm.CLI()
	if err != nil {
		t.Fatal(err)
	}
	mod, err := r.CompileModule(ctx, wasm)
	if err != nil {
		t.Fatal(err)
	}
	host := r.NewHostModuleBuilder("gophper")
	for _, f := range mod.ImportedFunctions() {
		module, name, _ := f.Import()
		if module != "gophper" {
			continue
		}
		results := f.ResultTypes()
		// No Go functions. Every other call fails.
		ret := api.EncodeI32(errnoNotSupported)
		if name == "fn_names" || name == "sig_take" {
			ret = 0
		}
		host.NewFunctionBuilder().
			WithGoModuleFunction(api.GoModuleFunc(func(_ context.Context, _ api.Module, stack []uint64) {
				if len(results) > 0 {
					stack[0] = ret
				}
			}), f.ParamTypes(), results).
			Export(name)
	}
	if _, err := host.Instantiate(ctx); err != nil {
		t.Fatal(err)
	}

	var out bytes.Buffer
	code := `echo PHP_VERSION, "\n", implode(",", get_loaded_extensions()), "\n", array_sum([20, 22]);`
	_, err = r.InstantiateModule(ctx, mod, wazero.NewModuleConfig().
		WithArgs("php", "-r", code).
		WithStdout(&out).
		WithStderr(&out))
	if exit, ok := err.(*sys.ExitError); ok && exit.ExitCode() == 0 {
		err = nil
	}
	if err != nil {
		t.Fatalf("%v\n%s", err, out.String())
	}

	lines := strings.Split(out.String(), "\n")
	if len(lines) != 3 || lines[0] != phpwasm.PHPVersion || lines[2] != "42" {
		t.Fatalf("unexpected output:\n%s", out.String())
	}
	loaded := strings.Split(lines[1], ",")
	for _, want := range []string{"openssl", "pdo_mysql", "pdo_pgsql", "pdo_sqlite", "dom", "mbstring", "zlib", "pcntl", "posix", "curl", "pgsql"} {
		found := false
		for _, l := range loaded {
			found = found || l == want
		}
		if !found {
			t.Errorf("%s is not loaded", want)
		}
	}
}

// TestDigests checks that the digests match the binaries.
func TestDigests(t *testing.T) {
	for _, tt := range []struct {
		bin    func() ([]byte, error)
		digest string
	}{{phpwasm.CLI, phpwasm.CLIDigest}, {phpwasm.CGI, phpwasm.CGIDigest}} {
		b, err := tt.bin()
		if err != nil {
			t.Fatal(err)
		}
		sum := sha256.Sum256(b)
		if got := hex.EncodeToString(sum[:8]); got != tt.digest {
			t.Errorf("digest %s, want %s", tt.digest, got)
		}
	}
}

// TestCGI only compiles php-cgi. Running it needs a CGI environment.
func TestCGI(t *testing.T) {
	ctx := context.Background()
	r := wazero.NewRuntimeWithConfig(ctx, wazero.NewRuntimeConfig().
		WithCoreFeatures(api.CoreFeaturesV2|experimental.CoreFeaturesExceptionHandling|experimental.CoreFeaturesExtendedConst))
	defer r.Close(ctx)
	wasm, err := phpwasm.CGI()
	if err != nil {
		t.Fatal(err)
	}
	if _, err := r.CompileModule(ctx, wasm); err != nil {
		t.Fatal(err)
	}
}

// TestExtensions checks that every extension keeps the dylink.0 section the
// host links with.
func TestExtensions(t *testing.T) {
	names := ext.Names()
	if len(names) == 0 {
		t.Fatal("no extensions")
	}
	for _, name := range names {
		b, err := ext.Open(name)
		if err != nil {
			t.Fatal(err)
		}
		if !bytes.HasPrefix(b, []byte("\x00asm")) || !bytes.Contains(b[:min(len(b), 64)], []byte("dylink.0")) {
			t.Errorf("%s: not a wasm side module with dylink.0", name)
		}
	}
}
