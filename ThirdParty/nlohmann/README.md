# nlohmann/json 3.11.3

This is the vendored header-only JSON implementation used by module metadata
encoding and decoding. The source archive is kept in
`ThirdParty/_downloads/nlohmann-json-3.11.3.tar.xz`; the upstream MIT license is
in `json/LICENSE.MIT`.

The engine adds `json/include` as a private include directory for `aeg_module`,
so normal engine and game-project consumers do not need to link a JSON target.
