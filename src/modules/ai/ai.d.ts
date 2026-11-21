/// <reference path="../../../src/types/prelude.d.ts" />
//@dts-block dyna:ai
/** dyna:ai — LLM API clients in native C over the shared dyna:http transport (http-call, httpk retry/backoff/SSE). Every network call runs on a fiber whose
 *  blocking I/O moves to a worker thread, so the event loop keeps serving while a request is in flight; there are no synchronous twins.
 *  Wire formats: the Claude Messages API (Anthropic), https://platform.claude.com/docs/en/api/messages/create.md and build-with-claude/streaming.md, and the
 *  OpenAI Chat Completions API (OpenAI, also served by ollama, llama.cpp and vLLM). The module never picks a
 *  model: every request names one (current IDs: claude-opus-5-5, claude-sonnet-5-5, claude-haiku-5-5). Request bodies are sent as given (JSON.stringify), so
 *  API-version rules are the server's: on anthropic-version 2026-10-21 temperature/top_p/top_k, budget_tokens thinking, a document block in an assistant
 *  message, or a text block with non-empty citations in a user message are 400s that surface as APIError, never silently dropped. */
declare module "dyna:ai" {
    /** A content block as the API returns it (text, thinking, redacted_thinking, tool_use, server_tool_use, server tool results, ...); unknown types pass through untouched. */
    interface ContentBlock {
        type: string;
        text?: string;
        thinking?: string;
        signature?: string;
        id?: string;
        name?: string;
        input?: Record<string, unknown>;
        citations?: unknown[];
        /** Set (non-enumerable, so JSON.stringify and a replayed request never carry it) on a streamed tool_use/server_tool_use block whose accumulated
         *  input_json_delta text is not a JSON object, which eager_input_streaming allows. `input` is then {}; `raw` is the text received. Answer it with a
         *  tool_result {is_error: true, content: JSON.stringify({INVALID_JSON: raw})} instead of running the tool. */
        readonly inputError?: { raw: string; message: string };
        [key: string]: unknown;
    }
    /** The Message object. stop_reason is one of "end_turn", "max_tokens", "stop_sequence", "tool_use", "pause_turn", "refusal" (stop_details is set only
     *  for "refusal") or null while streaming. Tool calls must not run when stop_reason is "max_tokens" or "refusal": the input can be cut off. */
    interface Message {
        id: string;
        type: "message";
        role: "assistant";
        model: string;
        content: ContentBlock[];
        stop_reason: "end_turn" | "max_tokens" | "stop_sequence" | "tool_use" | "pause_turn" | "refusal" | null;
        stop_sequence: string | null;
        stop_details?: { type: "refusal"; category: string | null; explanation?: string } | null;
        usage: { input_tokens: number; output_tokens: number; cache_creation_input_tokens?: number; cache_read_input_tokens?: number; [key: string]: unknown };
        [key: string]: unknown;
    }
    /** One streamed event exactly as the API sent it (the parsed data line): message_start, content_block_start, content_block_delta (text_delta,
     *  input_json_delta, thinking_delta, signature_delta, citations_delta), content_block_stop, message_delta, message_stop, ping, and any type added later. */
    interface StreamEvent {
        type: string;
        [key: string]: unknown;
    }
    /** Per-request options, merged over the client's: betas are appended to the client's anthropic-beta list, headers are appended after the client's. */
    interface RequestOptions {
        betas?: string[];
        headers?: Record<string, string>;
        /** Socket inactivity limit in ms (1..86400000); a long stream that keeps delivering events never hits it. */
        timeout?: number;
        /** 0..10. */
        maxRetries?: number;
        /** Cancels the call: on abort it rejects with APIError {status 0, type "aborted", retryable false} and the socket is shut down. An already
         *  aborted signal rejects before any I/O. An abort while the head is awaited, during a retry backoff sleep, or while a stream / batch results
         *  read is pending or idle takes effect at once (a later read rejects with "aborted", never a clean end); an abort during the TCP connect
         *  takes effect when the connect finishes or fails. A call that already settled is unaffected. The abort listener is removed when the call
         *  settles (a stream: when it ends or fails), so one long-lived signal can serve many calls. Anything without a boolean `aborted` and an
         *  addEventListener method is a TypeError; null means none. */
        signal?: AbortSignal | null;
    }
    /** The request body of POST /v1/messages (model, max_tokens, messages required; system, tools, tool_choice, thinking, output_config, metadata,
     *  stop_sequences, cache_control, ... are passed through unchanged). */
    interface MessageCreateParams {
        model: string;
        max_tokens: number;
        messages: unknown[];
        [key: string]: unknown;
    }
    /** An Anthropic Messages API client. Retries 408, 409, 429 and >= 500 answers and connection failures up to maxRetries times, waiting the server's
     *  retry-after-ms / retry-after when it is at most 60 s, else an exponential backoff from 500 ms (cap 8 s, +-25% jitter). A stream is retried only
     *  until its response head arrives; an error after that is thrown, never replayed. */
    class Anthropic implements DynResource {
        /** apiKey (x-api-key) or authToken (Authorization: Bearer) default to ANTHROPIC_API_KEY, then ANTHROPIC_AUTH_TOKEN; a request without either
         *  rejects with TypeError. baseURL defaults to ANTHROPIC_BASE_URL, then https://api.anthropic.com (a trailing / is dropped; a query is a
         *  TypeError). apiVersion is the anthropic-version header, default "2026-10-21"; pass "2023-06-01" to pin the older contract. maxRetries 0..10
         *  (default 2); timeout ms 1..86400000 (default 600000); betas joined into anthropic-beta; headers sent on every request; ca is a CA bundle path or
         *  inline PEM that replaces the platform trust store for https. An unknown option is a TypeError naming the valid ones. */
        constructor(opts?: { apiKey?: string; authToken?: string; baseURL?: string; apiVersion?: string; maxRetries?: number; timeout?: number;
            betas?: string[]; headers?: Record<string, string>; ca?: string });
        readonly baseURL: string;
        readonly apiVersion: string;
        readonly maxRetries: number;
        readonly timeout: number;
        readonly messages: {
            /** POST /v1/messages. Rejects with TypeError when model (there is no default), max_tokens or messages is missing, or when stream is true
             *  (use stream()); with APIError for an HTTP or transport failure. */
            create(body: MessageCreateParams, opts?: RequestOptions): Promise<Message>;
            /** POST /v1/messages with stream: true, sent on the first next()/finalMessage()/finalText(). The body's own `stream` key is ignored. */
            stream(body: MessageCreateParams, opts?: RequestOptions): MessageStream;
            /** POST /v1/messages/count_tokens; resolves {input_tokens}. max_tokens is not required. */
            countTokens(body: { model: string; messages: unknown[]; [key: string]: unknown }, opts?: RequestOptions): Promise<{ input_tokens: number; [key: string]: unknown }>;
            /** The Message Batches API, https://platform.claude.com/docs/en/build-with-claude/batch-processing.md (no beta header). A batch holds at most
             *  100000 requests or 256 MB; the server rejects params that set stream, so each params is a plain MessageCreateParams. */
            readonly batches: {
                /** POST /v1/messages/batches. Rejects with RangeError unless requests holds 1..100000 entries, and with TypeError for an entry without a
                 *  non-empty string custom_id or an object params (naming its index), before any I/O; params are sent as given. */
                create(body: { requests: Array<{ custom_id: string; params: MessageCreateParams }> }, opts?: RequestOptions): Promise<MessageBatch>;
                /** GET /v1/messages/batches/{id}; the id is one percent-encoded path segment (an empty or non-string id is a TypeError). */
                retrieve(id: string, opts?: RequestOptions): Promise<MessageBatch>;
                /** GET /v1/messages/batches with the given query; resolves the page, newest first. */
                list(query?: { limit?: number; after_id?: string; before_id?: string }, opts?: RequestOptions): Promise<{ data: MessageBatch[]; has_more: boolean; first_id: string | null; last_id: string | null }>;
                /** POST /v1/messages/batches/{id}/cancel (no body); processing_status becomes "canceling", then "ended". */
                cancel(id: string, opts?: RequestOptions): Promise<MessageBatch>;
                /** DELETE /v1/messages/batches/{id}; an in-progress batch must be canceled first (the API answers 400). */
                delete(id: string, opts?: RequestOptions): Promise<{ id: string; type: "message_batch_deleted" }>;
                /** GET /v1/messages/batches/{id}/results as a BatchResults iterator, connected on the first next(). Results arrive in any order: match
                 *  them by custom_id. Throws TypeError synchronously for a bad id. */
                results(id: string, opts?: RequestOptions): BatchResults;
            };
        };
        /** The Files API, https://platform.claude.com/docs/en/build-with-claude/files.md. It is generally available: no anthropic-beta header is sent
         *  (files-api-2025-04-14 is only needed for legacy shapes; pass it in betas if you want them). Reference an uploaded file from a message with
         *  {type: "document" | "image", source: {type: "file", file_id}}. */
        readonly files: {
            /** POST /v1/files as multipart/form-data: one "file" part (filename, and Content-Type only when mimeType is given) plus an
             *  "expires_in_seconds" field when expiresInSeconds is given. data is sent byte for byte (a string as UTF-8); the boundary is random and
             *  checked to be absent from data. Caller errors reject before any I/O: filename must be 1..255 characters with none of < > : " | ? * \ /
             *  or 0x00-0x1F (TypeError); mimeType must be a non-empty header-safe string (TypeError); expiresInSeconds must be an integer (TypeError)
             *  in 3600..7776000 (RangeError); an unknown key is a TypeError. The server caps a file at 500 MB (413 request_too_large). */
            upload(file: { data: Uint8Array | ArrayBuffer | ArrayBufferView | string; filename: string; mimeType?: string; expiresInSeconds?: number },
                opts?: RequestOptions): Promise<FileObject>;
            /** GET /v1/files. limit (default 20, at most 1000) and page (the previous page's next_page) page through the files; ids (at most 100,
             *  else RangeError; not combinable with page/limit) is sent as repeated ids[] parameters. Resolves {data, next_page}. */
            list(query?: { limit?: number; page?: string; ids?: string[] }, opts?: RequestOptions): Promise<{ data: FileObject[]; next_page: string | null; [key: string]: unknown }>;
            /** GET /v1/files/{id}. */
            retrieveMetadata(id: string, opts?: RequestOptions): Promise<FileObject>;
            /** GET /v1/files/{id}/content, resolved as a Uint8Array (at most 512 MiB). Only files the API created (downloadable: true) can be
             *  downloaded: an uploaded file answers 400 and an expired one 404, both as APIError. */
            download(id: string, opts?: RequestOptions): Promise<Uint8Array>;
            /** DELETE /v1/files/{id}. */
            delete(id: string, opts?: RequestOptions): Promise<{ id: string; type: "file_deleted" }>;
        };
        readonly models: {
            /** GET /v1/models with the given query (limit, after_id, before_id); resolves the page {data, has_more, first_id, last_id}. */
            list(query?: { limit?: number; after_id?: string; before_id?: string }, opts?: RequestOptions): Promise<{ data: Array<Record<string, unknown>>; has_more: boolean; first_id: string | null; last_id: string | null }>;
            /** GET /v1/models/{id}; the id is percent-encoded. */
            retrieve(id: string, opts?: RequestOptions): Promise<Record<string, unknown>>;
        };
        close(): void;
        dispose(): void;
        readonly closed: boolean;
        readonly [Symbol.dispose]: () => void;
    }
    /** A streamed Messages response: an async iterator of StreamEvent plus the accumulated Message. Text, thinking and partial_json deltas collect in
     *  native buffers (no per-delta string concatenation); a tool_use input is parsed strictly at its content_block_stop (see ContentBlock.inputError).
     *  thinking blocks keep their signature and are returned verbatim, ready to replay. One read at a time: a second next() while one is pending rejects
     *  with TypeError. An `event: error` rejects with the APIError it carries (overloaded_error is status 529, ...); a body that ends before
     *  message_stop, or data that is not a JSON object, rejects with APIError (status 0, type "connection_error" or "stream_error"). */
    class MessageStream implements DynResource, AsyncIterableIterator<StreamEvent> {
        /** Not constructible: use client.messages.stream(body). */
        private constructor();
        next(): Promise<IteratorResult<StreamEvent, undefined>>;
        /** Closes the connection (a for-await break calls it). */
        return(): Promise<IteratorResult<StreamEvent, undefined>>;
        [Symbol.asyncIterator](): MessageStream;
        /** Reads to message_stop and resolves the Message. */
        finalMessage(): Promise<Message>;
        /** finalMessage() with its text blocks concatenated. */
        finalText(): Promise<string>;
        /** The Message so far (text and thinking up to the last delta), or null before message_start. */
        readonly message: Message | null;
        /** The response status once connected, else null. */
        readonly status: number | null;
        /** The request-id response header once connected, else null. */
        readonly requestId: string | null;
        close(): void;
        dispose(): void;
        readonly closed: boolean;
        readonly [Symbol.dispose]: () => void;
    }
    /** A Message Batch. request_counts sum to the batch size; results_url is set once processing_status is "ended". */
    interface MessageBatch {
        id: string;
        type: "message_batch";
        processing_status: "in_progress" | "canceling" | "ended";
        request_counts: { processing: number; succeeded: number; errored: number; canceled: number; expired: number };
        ended_at: string | null;
        created_at: string;
        expires_at: string;
        cancel_initiated_at: string | null;
        results_url: string | null;
        [key: string]: unknown;
    }
    /** One line of a batch's results: succeeded carries the Message; errored carries the API error object ({type: "error", error: {type, message}}). */
    interface BatchResult {
        custom_id: string;
        result: { type: "succeeded"; message: Message } | { type: "errored"; error: { type: "error"; error: { type: string; message: string } } }
            | { type: "canceled" } | { type: "expired" };
    }
    /** A File object. downloadable is true only for files the API produced (for example by code execution); expires_at is null when the file does not expire. */
    interface FileObject {
        id: string;
        type: "file";
        filename: string;
        mime_type: string;
        size_bytes: number;
        created_at: string;
        downloadable: boolean;
        expires_at?: string | null;
        [key: string]: unknown;
    }
    /** A batch's JSONL results, streamed: each next() reads only as far as the next complete line and yields it parsed, so a large result set is never
     *  held in memory (one line is capped at 64 MiB). CRLF and blank lines are tolerated and a final line without a newline is accepted. A line that is
     *  not a JSON object rejects with APIError type "stream_error"; a read failure, or a Content-Length/chunked body that ends early, rejects with
     *  "connection_error"; a non-2xx answer rejects the first next() with its APIError. A cut never yields a partial line: a close-delimited body
     *  (no Content-Length, not chunked) cut on a line boundary is indistinguishable from its end. One read at a time, as MessageStream. */
    class BatchResults implements DynResource, AsyncIterableIterator<BatchResult> {
        /** Not constructible: use client.messages.batches.results(id). */
        private constructor();
        next(): Promise<IteratorResult<BatchResult, undefined>>;
        /** Closes the connection (a for-await break calls it). */
        return(): Promise<IteratorResult<BatchResult, undefined>>;
        [Symbol.asyncIterator](): BatchResults;
        /** The response status once connected, else null. */
        readonly status: number | null;
        /** The request-id response header once connected, else null. */
        readonly requestId: string | null;
        close(): void;
        dispose(): void;
        readonly closed: boolean;
        readonly [Symbol.dispose]: () => void;
    }
    /** The stream accumulator on its own: feed raw SSE bytes in any split (a recorded stream, a proxy) and read the Message. */
    class MessageAccumulator {
        constructor();
        /** Feeds bytes (UTF-8 string or Uint8Array) and returns the events completed by them, in order; throws what MessageStream would reject with. */
        feed(chunk: string | Uint8Array): StreamEvent[];
        /** The Message so far, or null before message_start. */
        readonly message: Message | null;
        /** True once message_stop was fed. */
        readonly done: boolean;
    }
    /** A Chat Completions message as the API returns it. content is null when the turn is only tool calls (or a refusal); reasoning (ollama,
     *  llama.cpp) or reasoning_content (vLLM, DeepSeek) appears only when the server sends it. tool_calls[i].function.arguments is the JSON text the
     *  model produced, unparsed: JSON.parse it and answer a parse failure with a tool message instead of running the tool. Unknown keys pass through. */
    interface ChatCompletionMessage {
        role: "assistant";
        content: string | null;
        refusal: string | null;
        reasoning?: string;
        reasoning_content?: string;
        tool_calls?: Array<{ id: string; type: "function"; function: { name: string; arguments: string } }>;
        [key: string]: unknown;
    }
    /** The ChatCompletion object of POST /chat/completions. finish_reason is "stop", "length" (max_tokens/max_completion_tokens hit: tool call
     *  arguments may be cut off, so do not run them), "tool_calls", "content_filter" or null while streaming. usage is null in a stream that did not
     *  ask for it. */
    interface ChatCompletion {
        id: string;
        object: "chat.completion";
        created: number;
        model: string;
        choices: Array<{ index: number; message: ChatCompletionMessage; finish_reason: "stop" | "length" | "tool_calls" | "content_filter" | null;
            logprobs: { content?: unknown[]; refusal?: unknown[] } | null; [key: string]: unknown }>;
        usage: { prompt_tokens: number; completion_tokens: number; total_tokens: number; [key: string]: unknown } | null;
        [key: string]: unknown;
    }
    /** One streamed chunk exactly as the server sent it (the parsed data line); the final usage chunk has choices: []. */
    interface ChatCompletionChunk {
        id: string;
        object: "chat.completion.chunk";
        created: number;
        model: string;
        choices: Array<{ index: number; delta: Record<string, unknown>; finish_reason: string | null; [key: string]: unknown }>;
        usage?: { prompt_tokens: number; completion_tokens: number; total_tokens: number; [key: string]: unknown } | null;
        [key: string]: unknown;
    }
    /** The request body of POST /chat/completions (model and messages required; tools, tool_choice, response_format, max_completion_tokens,
     *  max_tokens, temperature, n, reasoning_effort, ... are passed through unchanged: the server decides what it accepts). */
    interface ChatCompletionCreateParams {
        model: string;
        messages: unknown[];
        [key: string]: unknown;
    }
    /** Per-request options for OpenAI, merged over the client's (there is no betas key: it is a TypeError). */
    interface OpenAIRequestOptions {
        headers?: Record<string, string>;
        /** Socket inactivity limit in ms (1..86400000). */
        timeout?: number;
        /** 0..10. */
        maxRetries?: number;
        /** As RequestOptions.signal. */
        signal?: AbortSignal | null;
    }
    /** A client for the OpenAI Chat Completions API and the servers that speak it (ollama, llama.cpp llama-server, vLLM, LM Studio, ...), wire
     *  format https://platform.openai.com/docs/api-reference/chat. Same transport, fiber offload, retry and error rules as Anthropic: retries 408, 409,
     *  429, >= 500 and connection failures up to maxRetries, honouring retry-after-ms / retry-after up to 60 s; a stream is retried only until its
     *  response head arrives. The module never picks a model. */
    class OpenAI implements DynResource {
        /** apiKey (sent as Authorization: Bearer) defaults to OPENAI_API_KEY; a request without one rejects with TypeError (a local server that
         *  ignores keys still needs a placeholder). baseURL is the API root including its version path, default OPENAI_BASE_URL, then
         *  https://api.openai.com/v1 (for ollama: http://127.0.0.1:11434/v1; a trailing / is dropped; a query is a TypeError). organization and project
         *  (defaults OPENAI_ORG_ID, OPENAI_PROJECT_ID) are sent as OpenAI-Organization / OpenAI-Project when set. maxRetries, timeout, headers and ca as
         *  for Anthropic. An unknown option (apiVersion, betas, authToken, ...) is a TypeError naming the valid ones. */
        constructor(opts?: { apiKey?: string; baseURL?: string; organization?: string; project?: string; maxRetries?: number; timeout?: number;
            headers?: Record<string, string>; ca?: string });
        readonly baseURL: string;
        readonly maxRetries: number;
        readonly timeout: number;
        readonly chat: {
            readonly completions: {
                /** POST {baseURL}/chat/completions. Rejects with TypeError when model (there is no default) is missing or empty, messages is not an
                 *  array, or stream is true (use stream()); with APIError for an HTTP or transport failure (type is the body's error.type, e.g.
                 *  invalid_request_error; body.error.code and body.error.param are on APIError.body). */
                create(body: ChatCompletionCreateParams, opts?: OpenAIRequestOptions): Promise<ChatCompletion>;
                /** POST {baseURL}/chat/completions with stream: true, sent on the first next()/finalChatCompletion()/finalContent(). The body's own
                 *  `stream` key is ignored. When the body has no stream_options key, stream_options {include_usage: true} is added so the completion
                 *  carries usage; a stream_options the caller gives (even null) is sent as given. */
                stream(body: ChatCompletionCreateParams, opts?: OpenAIRequestOptions): ChatCompletionStream;
            };
        };
        readonly models: {
            /** GET {baseURL}/models with the given query; resolves {object: "list", data}. */
            list(query?: Record<string, string | number | boolean>, opts?: OpenAIRequestOptions): Promise<{ data: Array<Record<string, unknown>>; [key: string]: unknown }>;
            /** GET {baseURL}/models/{id}; the id is one percent-encoded path segment (an empty or non-string id is a TypeError). */
            retrieve(id: string, opts?: OpenAIRequestOptions): Promise<Record<string, unknown>>;
        };
        readonly embeddings: {
            /** POST {baseURL}/embeddings. model (no default) and input (a string or an array of strings or token arrays) are required, else
             *  TypeError; dimensions, user, ... pass through. When the body has no encoding_format key the client sends encoding_format "base64"
             *  (4 bytes per dimension on the wire instead of a JSON number) and resolves every data[i].embedding as a Float32Array decoded natively
             *  (little-endian float32, bit-exact); a server that ignores the request and answers number arrays also yields Float32Array (each
             *  value rounded to float32). An explicit encoding_format ("float" or "base64") is sent as given and the response is returned
             *  untouched. A 2xx answer without a data array, or an embedding that is bad base64, not a multiple of 4 bytes, holds a non-number
             *  or is of another type, rejects with APIError type "api_error" naming data[i]. */
            create(body: { model: string; input: string | string[] | number[] | number[][]; encoding_format?: "float" | "base64"; dimensions?: number;
                [key: string]: unknown }, opts?: OpenAIRequestOptions): Promise<{ object: "list"; model: string;
                data: Array<{ object: "embedding"; index: number; embedding: Float32Array | number[] | string }>;
                usage: { prompt_tokens: number; total_tokens: number }; [key: string]: unknown }>;
        };
        close(): void;
        dispose(): void;
        readonly closed: boolean;
        readonly [Symbol.dispose]: () => void;
    }
    /** A streamed Chat Completions response: an async iterator of ChatCompletionChunk plus the accumulated ChatCompletion. Content, refusal,
     *  reasoning/reasoning_content and tool call arguments collect in native buffers per choice (no per-delta string concatenation); tool calls are
     *  keyed by their index (by array position when a server omits it), take id/type/name from the first piece that has them and concatenate
     *  arguments; logprobs arrays concatenate; other delta keys are copied onto the message; usage comes from the choices-less final chunk. Only
     *  `data: [DONE]` ends the stream cleanly: a body that ends before it rejects with APIError "connection_error" (even after every finish_reason),
     *  a data line holding {error: {...}} rejects with the APIError it carries, a data line that is not a JSON object or a choice/tool call index
     *  outside 0..1023 rejects with "stream_error". One read at a time, as MessageStream. */
    class ChatCompletionStream implements DynResource, AsyncIterableIterator<ChatCompletionChunk> {
        /** Not constructible: use client.chat.completions.stream(body). */
        private constructor();
        next(): Promise<IteratorResult<ChatCompletionChunk, undefined>>;
        /** Closes the connection (a for-await break calls it). */
        return(): Promise<IteratorResult<ChatCompletionChunk, undefined>>;
        [Symbol.asyncIterator](): ChatCompletionStream;
        /** Reads to [DONE] and resolves the ChatCompletion. */
        finalChatCompletion(): Promise<ChatCompletion>;
        /** finalChatCompletion().choices[0].message.content (null when that choice has no content). */
        finalContent(): Promise<string | null>;
        /** The ChatCompletion so far, or null before the first chunk. */
        readonly completion: ChatCompletion | null;
        /** The response status once connected, else null. */
        readonly status: number | null;
        /** The x-request-id (or request-id) response header once connected, else null. */
        readonly requestId: string | null;
        close(): void;
        dispose(): void;
        readonly closed: boolean;
        readonly [Symbol.dispose]: () => void;
    }
    /** The Chat Completions stream accumulator on its own: feed raw SSE bytes in any split and read the ChatCompletion. */
    class ChatCompletionAccumulator {
        constructor();
        /** Feeds bytes (UTF-8 string or Uint8Array) and returns the chunks completed by them, in order (never [DONE]; bytes after [DONE] are
         *  ignored); throws what ChatCompletionStream would reject with. */
        feed(chunk: string | Uint8Array): ChatCompletionChunk[];
        /** The ChatCompletion so far, or null before the first chunk. */
        readonly completion: ChatCompletion | null;
        /** True once data: [DONE] was fed. */
        readonly done: boolean;
    }
    /** Every dyna:ai failure that is not a caller error (those are TypeError/RangeError). status: the HTTP status, or 0 for a transport or stream failure.
     *  type: the API's error.type (invalid_request_error 400, authentication_error 401, billing_error 402, permission_error 403, not_found_error 404,
     *  request_too_large 413, rate_limit_error 429, api_error 500, overloaded_error 529), else "connection_error", "stream_error", or "aborted" (the
     *  request's signal fired).
     *  For OpenAI, type is the body's error.type as sent (invalid_request_error, server_error, ...; a mid-stream error object has status 0).
     *  requestId: the body's request_id or the request-id header, else x-request-id. retryAfterMs: from retry-after-ms / retry-after, else null. retryable: true for 408, 409,
     *  429, >= 500 and connection errors. body: the parsed error body; headers: the response headers. */
    class APIError extends Error {
        constructor(message?: string, init?: { status?: number; type?: string; requestId?: string; retryAfterMs?: number; headers?: Record<string, string>; body?: unknown });
        readonly status: number;
        readonly type: string;
        readonly requestId: string | null;
        readonly retryAfterMs: number | null;
        readonly retryable: boolean;
        readonly headers: Record<string, string> | null;
        readonly body: unknown;
    }
}
