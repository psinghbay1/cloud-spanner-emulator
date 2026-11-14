//
// Copyright 2020 Google LLC
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
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

func TestEmulatorArgsForwardSchemaVersionRetention(t *testing.T) {
	gw := New(Options{FrontendAddress: "localhost:9010", SchemaVersionRetention: "0s"})
	args := gw.emulatorArgs()
	if !slices.Contains(args, "--schema_version_retention=0s") {
		t.Errorf("schema_version_retention not forwarded: %v", args)
	}
}

func TestEmulatorArgsOmitUnsetSchemaVersionRetention(t *testing.T) {
	gw := New(Options{FrontendAddress: "localhost:9010"})
	for _, arg := range gw.emulatorArgs() {
		if strings.HasPrefix(arg, "--schema_version_retention") {
			t.Errorf("unset retention must not be forwarded: %v", arg)
		}
	}
}
