//
// Copyright 2026 Google LLC
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//      http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.
//

package gateway

import (
	"slices"
	"strings"
	"testing"
)

func TestEmulatorArgsForwardStoragePurgeInterval(t *testing.T) {
	gw := New(Options{FrontendAddress: "localhost:9010", StoragePurgeInterval: "10m"})
	if args := gw.emulatorArgs(); !slices.Contains(args, "--storage_purge_interval=10m") {
		t.Errorf("storage_purge_interval not forwarded: %v", args)
	}
}

func TestEmulatorArgsOmitUnsetStoragePurgeInterval(t *testing.T) {
	gw := New(Options{FrontendAddress: "localhost:9010"})
	for _, arg := range gw.emulatorArgs() {
		if strings.HasPrefix(arg, "--storage_purge_interval") {
			t.Errorf("unset interval must not be forwarded: %v", arg)
		}
	}
}

func TestEmulatorArgsForwardDiagnosticStatements(t *testing.T) {
	gw := New(Options{FrontendAddress: "localhost:9010", EnableEmulatorDiagnosticStatements: true})
	if args := gw.emulatorArgs(); !slices.Contains(args, "--enable_emulator_diagnostic_statements") {
		t.Errorf("enable_emulator_diagnostic_statements not forwarded: %v", args)
	}
}

func TestEmulatorArgsOmitDiagnosticStatementsByDefault(t *testing.T) {
	gw := New(Options{FrontendAddress: "localhost:9010"})
	if args := gw.emulatorArgs(); slices.Contains(args, "--enable_emulator_diagnostic_statements") {
		t.Errorf("diagnostic statements must be off by default: %v", args)
	}
}
