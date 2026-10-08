-- wrk_rpc.lua -- the load script tests/bench_http_app.js documents in its own
-- usage line, and which had never been checked in: the invocation printed in
-- the file could not be run, which is the hardcoded-probe defect class in
-- another form (CLAUDE.md sec 13/14).
--
-- Drives App.rpc with a strict JSON-RPC 2.0 request whose handler BUILDS a
-- five-field object, so the response path does real JSON.stringify work rather
-- than echoing a constant. `ping` would measure the envelope only.
wrk.method = "POST"
wrk.headers["Content-Type"] = "application/json"
wrk.body = '{"jsonrpc":"2.0","method":"record","params":[7],"id":1}'
