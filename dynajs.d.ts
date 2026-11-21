/** DynaJS types: every `dyna:*` module, the web globals, `std`/`os` (with --std) and the prototype extensions (removed by --no-prototypes).
 *  Option bags are strict: an unknown key throws TypeError. Untrusted input is capped and refused, never truncated. There is no `Intl`. */

// ---- Shared helper types ----

/** A byte-addressed view: a typed array of 1-byte elements, a DataView, or an ArrayBuffer. */
type ByteView = Uint8Array | Int8Array | Uint8ClampedArray | DataView | ArrayBuffer;

/** Bytes input: a string (encoded as UTF-8) or a byte view. BORROW RULE: the buffer is on loan for the call; resize/transfer from a hook mid-call throws TypeError. */
type BytesInput = string | ByteView;

/** 4-byte typed arrays the f32 SIMD kernels accept; Int32Array/Uint32Array are BIT-CAST (raw 32-bit patterns read as IEEE floats), never converted. */
type F32Like = Float32Array | Int32Array | Uint32Array;

/** Any typed array, all element widths; DataView and ArrayBuffer are NOT included (byte-filling natives such as Random.fill refuse them). */
type AnyTypedArray = Int8Array | Uint8Array | Uint8ClampedArray | Int16Array | Uint16Array
    | Int32Array | Uint32Array | Float32Array | Float64Array | BigInt64Array | BigUint64Array;

/** Integer typed arrays: the only kinds the Atomics read-modify-write ops accept (BigInt kinds take/return BigInt). */
type AnyIntegerTypedArray = Int8Array | Uint8Array | Uint8ClampedArray | Int16Array | Uint16Array
    | Int32Array | Uint32Array | BigInt64Array | BigUint64Array;

/** Decimal rounding modes (IEEE 754-2008 decimal128 names). */
type RoundingMode = "up" | "down" | "ceil" | "floor" | "halfUp" | "halfDown" | "halfEven" | "halfOdd";

/** Segment-tree fold operations. */
type SegOp = "sum" | "min" | "max";

/** A native resource: released explicitly or by the GC finalizer. */
interface DynResource {
    close(): void;
    dispose(): void;
    readonly closed: boolean;
    readonly [Symbol.dispose]: () => void;
}

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

/** dyna:bytes — copied byte buffers (Bytes), UTF-8-safe strings (Text), search and fixed-width reads/writes; every method is also a free function taking the buffer first.
 *  METHOD <-> FREE-FUNCTION MAP: same names except Bytes.equals(v) <-> equal(a, b) and includes <-> contains; method fill returns the handle, free fill the view. */
declare module "dyna:bytes" {
    /** Copied byte buffer: construction, slicing, search, text interpretation; fixed-width access is a DataView over `array`. */
    class Bytes {
        /** The bytes are copied, never aliased, so a later write through the source cannot invalidate the cached `isAscii`/`isValidUtf8` flags. */
        constructor(data: string | ByteView);
        /** Zero-filled buffer; n coerces like ToIndex (NaN -> 0, 3.7 -> 3); a negative or Infinity n is TypeError, above 2^31-1 RangeError. */
        static alloc(n: number): Bytes;
        /** True when `v` is a Bytes handle. */
        static isBytes(v: unknown): v is Bytes;
        /** One allocation, sized in a first pass; every element must be a byte-addressed view. */
        static concat(list: ByteView[]): Bytes;
        /** Variadic form; a leading array and further views mix freely. */
        static concat(...views: ByteView[]): Bytes;
        /** The byte count. */
        get length(): number;
        /** True when no byte has the high bit set; cached at construction, recomputed once `array` has been read, so writes through it are seen. */
        get isAscii(): boolean;
        /** True when the bytes are well-formed UTF-8, cached like isAscii; also a getter on Text and the free function isValidUtf8(data). */
        get isValidUtf8(): boolean;
        /** The live backing Uint8Array, bounded to this handle's bytes (a slice's array starts and ends with the slice). */
        get array(): Uint8Array;
        /** A new Bytes handle that is a view sharing the owner's ArrayBuffer. */
        slice(start?: number, end?: number): Bytes;
        /** Lexicographic byte comparison; -1, 0, or 1. */
        compare(other: ByteView): number;
        /** True when the other view has identical length and bytes. */
        equals(other: ByteView): boolean;
        /** First position of a byte value (0..255) or byte view from `fromIndex` (negative clamps to 0); -1 when absent. */
        indexOf(needle: number | ByteView, fromIndex?: number): number;
        /** Last position of the needle, searching backward from `fromIndex`; the empty needle matches at `length`. */
        lastIndexOf(needle: number | ByteView, fromIndex?: number): number;
        /** True when the needle occurs. */
        includes(needle: number | ByteView): boolean;
        /** Count of non-overlapping occurrences from `fromIndex`; the empty needle counts `length - fromIndex + 1`. */
        count(needle: number | ByteView, fromIndex?: number): number;
        /** First position at or after `fromIndex` holding any byte of `chars`, or -1. */
        indexOfAny(chars: ByteView, fromIndex?: number): number;
        /** True when the needle (byte value or view) sits exactly at `fromIndex` (default 0). */
        startsWith(needle: number | ByteView, fromIndex?: number): boolean;
        /** True when the needle ENDS exactly at `end` (exclusive; default `length`). */
        endsWith(needle: number | ByteView, end?: number): boolean;
        toString(): string;
    }

    /** Wraps a JS string and caches isWide in one scan at construction. */
    class Text {
        /** Wraps a JS string and caches `isWide` (whether any code unit is above U+00FF) in one scan at construction. */
        constructor(s: string);
        /** True when any code unit is above U+00FF. */
        get isWide(): boolean;
        /** The wrapped string. */
        get value(): string;
        /** Method-vs-getter note: a GETTER, not a method — true when the string encodes to well-formed UTF-8 (it holds no lone surrogate). */
        get isValidUtf8(): boolean;
        /** True when the string has no lone surrogate. */
        isValidUtf16(): boolean;
        /** UTF-8 code points of the string. */
        countUtf8(): number;
        /** Code points, surrogate pairs counted once. */
        countUtf16(): number;
        /** Each input byte as a Latin-1 code point re-encoded to UTF-8. */
        latin1ToUtf8(): Uint8Array;
        /** Throws RangeError on invalid UTF-8 or any code point above 0xFF. */
        utf8ToLatin1(): Uint8Array;
        /** UTF-16LE bytes from UTF-8; strict, throws RangeError on malformed input. */
        utf8ToUtf16(): Uint8Array;
        /** Strict/lossless UTF-16LE to UTF-8; throws on odd length or an ill-formed surrogate. */
        utf16ToUtf8(): Uint8Array;
        /** The string as a Bytes handle. */
        toBytes(): Bytes;
        toJSON(): string;
        toString(): string;
    }

    /** Lexicographic byte comparison; -1, 0, or 1. */
    function compare(a: ByteView, b: ByteView): number;
    /** True for identical bytes. */
    function equal(a: ByteView, b: ByteView): boolean;
    /** First position of the needle (byte value or view) from `fromIndex`, or -1. */
    function indexOf(buf: ByteView, needle: number | ByteView, fromIndex?: number): number;
    /** Last position of the needle, searching backward from `fromIndex`. */
    function lastIndexOf(buf: ByteView, needle: number | ByteView, fromIndex?: number): number;
    /** True when the needle occurs. */
    function contains(buf: ByteView, needle: number | ByteView): boolean;
    /** Number of non-overlapping occurrences; the scan starts at `fromIndex`. */
    function count(buf: ByteView, needle: number | ByteView, fromIndex?: number): number;
    /** Concatenates byte views into one Uint8Array. */
    function concat(list: ByteView[]): Uint8Array;
    /** Overlap-safe byte copy returning the number of bytes copied. */
    function copy(dst: Uint8Array, src: ByteView, dstOff?: number, srcOff?: number, len?: number): number;
    /** Well-formed UTF-8 check over a string or byte view. */
    function isValidUtf8(data: BytesInput): boolean;
    /** Well-formed UTF-16LE check (even length, paired surrogates). */
    function isValidUtf16(u16bytes: ByteView): boolean;
    /** UTF-8 code points (assumes valid UTF-8). */
    function countUtf8(data: BytesInput): number;
    /** Code points, pairs once, no validation. */
    function countUtf16(u16bytes: ByteView): number;
    /** Each input byte as a Latin-1 code point re-encoded to UTF-8. */
    function latin1ToUtf8(bytes: ByteView): Uint8Array;
    /** Throws RangeError on invalid UTF-8 or code points above 0xFF. */
    function utf8ToLatin1(bytes: ByteView): Uint8Array;
    /** Strict UTF-8 to UTF-16LE; throws on malformed input. */
    function utf8ToUtf16(bytesOrString: BytesInput): Uint8Array;
    /** Strict UTF-16LE to UTF-8; throws on odd length or an ill-formed surrogate. */
    function utf16ToUtf8(u16bytes: ByteView): Uint8Array;
    /** Decodes a byte view from a legacy single-byte charset; unknown label throws RangeError. */
    function decode(bytes: ByteView, label: string): string;
    /** Encodes a string into a byte view; unexpressible code points become `?`. For utf-8 a lone surrogate becomes U+FFFD (EF BF BD), as TextEncoder does. */
    function encode(text: string, label: string): Uint8Array;
    /** True for every built label plus utf-8/utf8; ASCII case-insensitive. */
    function encodingExists(label: string): boolean;
    /** The array of every label this build can decode, beginning with utf-8. */
    function encodings(): string[];

}

/** dyna:cli — terminal programs: argument parsing (Command), ANSI styling, prompts, raw-mode key input, progress bars. */
declare module "dyna:cli" {
    // $DYNAJS_BYTECODE_CACHE is a developer speedup over TRUSTED input: never enable it for scripts in directories writable by others.

    /** Styles text with a named ANSI style or list (also "256:n", "#rrggbb", "rgb:r,g,b" and bg forms); escapes drop under $NO_COLOR or a non-TTY; unknown names throw. */
    function StyleText(style: string | string[], text: string): string;
    /** The list of styles the engine can apply. */
    function Styles(): string[];
    /** True when the stream with the given fd (default stdout) is a TTY. */
    function IsTTY(fd?: number): boolean;
    /** Terminal columns: TIOCGWINSZ on fd (stdout default), else $COLUMNS, else 80. */
    function Columns(fd?: number): number;
    /** The terminal color depth: 0 (none), 4 (16 colors), 8 (256), or 24 (truecolor). */
    function ColorDepth(fd?: number): number;

    /** Prints `message` and reads one stdin line; empty or EOF yields `opts.default` (else ""); an answer over 1 MiB is a RangeError. */
    function prompt(message: string, opts?: { default?: string }): string;
    /** Prints `message` and reads a line: "y"/"yes" (case-insensitive) is true, anything else including EOF is false. */
    function confirm(message: string): boolean;
    /** Arrow-key menu in raw mode; returns the chosen option, or null on Escape/Ctrl-C/EOF. The terminal mode is restored on every exit path. */
    function select(message: string, options: string[]): string | null;
    /** Reads one key event in raw mode; null at EOF. A lone ESC is told from a sequence head by a 50 ms quiet window. */
    function keypress(): {
        /** "up", "enter", "a", ... -- null for an unrecognized sequence. */
        name: string | null;
        /** The bytes the event consumed, UTF-8 decoded (malformed runs appear as U+FFFD). */
        sequence: string;
        ctrl: boolean;
        meta: boolean;
        shift: boolean;
    } | null;

    /** A single-line progress bar with a time-based ETA. */
    class ProgressBar {
        constructor(opts: { total: number });
        /** Redraws the bar at `n` of `total`; reaching `total` finishes the line and later updates are refused. */
        update(n: number): this;
    }

    /** A single-line spinner over the frames "|/-\". */
    class Spinner {
        constructor(opts?: { text?: string });
        /** Starts the animation; chainable. */
        start(): this;
        /** Advances one frame and redraws. */
        tick(): this;
        /** Clears the line, or leaves `text` as the final line (with a newline). */
        stop(text?: string): this;
    }

    /** Renders rows as aligned columns ("grid"), TSV, or RFC 4180 CSV. */
    function Table(rows: (string | number)[][], opts?: {
        /** Header row, rendered first ("grid" rules it off with dashes). */
        head?: string[];
        /** Per-column alignment; "left" is the default for unlisted columns. */
        align?: ("left" | "right" | "center")[];
        /** "grid" (default), "tsv", or "csv". */
        format?: "grid" | "tsv" | "csv";
    }): string;

    /** A command-line parser: options, positional arguments, subcommands, generated help. */
    class Command {
        /** Constructs an argument parser built from declarative specs. */
        constructor(name?: string);
        get name(): string;
        /** Describes this command for help output. */
        describe(text: string): this;
        /** Registers an option; precedence is CLI > `env` variable > default. Numbers use a strict finite-decimal grammar ("NaN", "0x10" are refused). */
        option(flags: string, description?: string, opts?: { type?: "boolean" | "string" | "number"; required?: boolean; variadic?: boolean; default?: unknown; env?: string }): this;
        /** Registers a positional argument. */
        argument(name: string, description?: string): this;
        /** Registers a subcommand. */
        command(sub: Command): this;
        /** Permits unknown options instead of refusing them. */
        allowUnknown(v: boolean): this;
        /** Registers the handler a leaf parse calls with (options, args); its return lands on the parse result's `result`. */
        action(fn: (options: Record<string, unknown>, args: string[]) => unknown): this;
        /** Sets the version and enables `--version` (chainable); with no argument returns the version ("" when unset). */
        version(v: string): this;
        version(): string;
        /** Parses argv (default scriptArgs) into { options, arguments, command }; a subcommand's own parse nests under `result`, so a leaf action's return is `.result.result`. Pass T to narrow. A NUL byte in argv is a TypeError. */
        parse<T = { options: Record<string, unknown>; arguments: string[]; command: string | null }>(args?: string[]): T;
        /** Prints the help text. */
        help(): string;
    }
}

/** dyna:compress — one-shot codecs (zstd, brotli, gzip, deflate, snappy, LZ4), tar and zip archives, and reusable Compressor/Dictionary objects.
 *  Decompressors take maxOutputBytes (1..1 GiB, exact; exceeding it is a RangeError) and refuse trailing bytes, so bombs fail deterministically. */
declare module "dyna:compress" {
    /** Zstandard compression; level 1..22. Frames carry a content checksum (4 bytes; `checksum: false` omits it), so unzstd refuses a corrupted body. */
    function zstd(data: BytesInput, opts?: { level?: number; checksum?: boolean }): Uint8Array;
    /** Zstandard decompression; malformed input is refused. Output is capped at 1 GiB per call; an over-cap stream is refused, never truncated. */
    function unzstd(data: ByteView, opts?: { asString?: boolean; maxOutputBytes?: number }): Uint8Array | string;
    /** Brotli, level 0..11; on macOS the system codec has one fixed effort, so `level` is validated and otherwise ignored. */
    function brotli(data: BytesInput, opts?: { level?: number }): Uint8Array;
    /** Brotli decompression. */
    function unbrotli(data: ByteView, opts?: { asString?: boolean; maxOutputBytes?: number }): Uint8Array | string;
    /** Snappy block compression. */
    function snappy(data: BytesInput): Uint8Array;
    /** Snappy block decompression. */
    function unsnappy(data: ByteView, opts?: { asString?: boolean; maxOutputBytes?: number }): Uint8Array | string;
    /** Raw LZ4 block (no header); `dict` seeds the match window; level 1..12 (clamped by the codec). */
    function lz4Compress(data: BytesInput, opts?: { level?: number; dict?: ByteView }): Uint8Array;
    /** Raw LZ4 block decompression; `dict` must be the dictionary used at compress time. */
    function lz4Decompress(data: ByteView, opts?: { level?: number; dict?: ByteView; maxOutputBytes?: number }): Uint8Array;
    /** LZ4 frame format with optional content checksum. */
    function lz4Frame(data: BytesInput, opts?: { level?: number; checksum?: boolean }): Uint8Array;
    /** LZ4 frame decompression; a bad checksum or structure is refused. */
    function lz4Unframe(data: ByteView, opts?: { asString?: boolean; maxOutputBytes?: number }): Uint8Array | string;
    /** RFC 1952 gzip framing. An absent level is 6, as in zlib; it was 1 before, so this is a behaviour change: a caller that wants the old speed and output must pass level 1 explicitly (level 1 output is byte-identical to before). Levels 1..5 (0, negatives and NaN count as 1) are greedy over progressively deeper hash chains and use fixed Huffman below 8 KiB; 6..9 match lazily as zlib does, over chains of 128, 256, 1024 and 4096, and a 3-byte match more than 4 KiB back is coded as literals; above 9 counts as 9. Measured on text, CSV and binary corpora, level 9 output is no larger than zlib -9 and level 6 is within 0.01% of zlib -6. At 6..9 every block of 16384 tokens is coded stored, fixed or dynamic, whichever is smallest, so incompressible stretches cost 5 bytes per 64 KiB; 1..5 code the whole input as one block. Out-of-range levels are not refused. Output keeps a 16-byte slack requirement. */
    function gzip(data: BytesInput, level?: number): Uint8Array;
    /** gzip with the level in an options bag (same encoder as the positional form); a non-number level is a TypeError, absent means level 6. */
    function gzip(data: BytesInput, opts?: { level?: number }): Uint8Array;
    /** RFC 1952 gzip, inverse of gzip above; CRC32 + ISIZE validated, a corrupted stream is refused. */
    function gunzip(data: ByteView, opts?: { asString?: boolean; maxOutputBytes?: number }): Uint8Array | string;

    /** Worst-case compressed size of `n` input bytes for one codec, so an output buffer can be allocated once. */
    function compressBound(n: number, opts: { algo: CompressorAlgo }): number;

    /** ustar archive entry for packing. */
    interface TarEntry {
        name: string;
        data?: ByteView;
        /** "file" or "directory" (link types are refused); absent means "directory" iff there is no data. */
        type?: string;
        mode?: number;
        mtime?: number;
    }
    /** Metadata record read from an archive. */
    interface TarRecord {
        name: string;
        size: number;
        mtime: number;
        mode: number;
        type: "file" | "directory" | "symlink" | "link" | "device" | "fifo";
        linkname?: string;
        data?: Uint8Array;
    }
    /** Writes a ustar archive. Names must be relative, <= 4096 bytes, with no NUL or `..` component (TarList/TarExtract apply the same gate unless allowUnsafeNames). */
    function TarPack(entries: TarEntry[]): Uint8Array;
    /** Reads archive metadata; `allowUnsafeNames` lifts the safe-name check. */
    function TarList(bytes: ByteView, opts?: { allowUnsafeNames?: boolean }): TarRecord[];
    /** The TarList records with `data` added to every non-directory entry. */
    function TarExtract(bytes: ByteView, opts?: { allowUnsafeNames?: boolean }): TarRecord[];

    /** Zip entry to pack: optional method, mtime (unix s), mode (default 0644), comment; "deflate" is ADVISORY — a member that would not shrink is stored. */
    interface ZipEntry {
        name: string;
        data: ByteView;
        method?: "deflate" | "store";
        mtime?: number;
        mode?: number;
        comment?: string;
    }
    /** Central-directory listing record (mtime in unix seconds, 0 = none). */
    interface ZipRecord {
        name: string;
        size: number;
        mtime: number;
        mode: number;
        comment: string;
        type: string;
        compressedSize: number;
        crc32: number;
        method: string;
    }
    /** Zip member with its bytes (ZipExtractAll/ZipReadAt shape). */
    interface ZipMember extends ZipRecord {
        data: Uint8Array;
    }
    /** Writes a zip archive; method "deflate" or "store". */
    function ZipPack(entries: ZipEntry[], opts?: { method?: "deflate" | "store" }): Uint8Array;
    /** Central-directory listing. Lists exactly the number of entries the end record declares; a directory holding more is a SyntaxError. */
    function ZipList(bytes: ByteView, opts?: { allowUnsafeNames?: boolean }): ZipRecord[];
    /** Extracts one member by exact name (CRC and size verified); each call walks the directory, so use ZipExtractAll to read every member. */
    function ZipRead(bytes: ByteView, name: string, opts?: { allowUnsafeNames?: boolean }): Uint8Array;
    /** Entry by-index in central-directory order; out of range is a RangeError. */
    function ZipReadAt(bytes: ByteView, index: number, opts?: { allowUnsafeNames?: boolean }): ZipMember;
    /** Options of ZipExtractAll. */
    interface ZipExtractOptions {
        /** Destination directory (string or Path); absent = no writes. */
        dir?: string | import("dyna:file").Path;
        /** Opt-in escape: names containing `..` are then written OUTSIDE `dir`; without it such archives are refused. */
        allowUnsafeNames?: boolean;
    }
    /** Every member with its bytes; given a destination, also written under it. ALL OR NOTHING: names, sizes and CRCs are validated before any file is created.
     *  Refused: absolute names, backslashes, NUL, `..`, symlinks under the destination; output is capped at 1 GiB across all members and per member. */
    function ZipExtractAll(bytes: ByteView, dir?: string | import("dyna:file").Path | ZipExtractOptions, opts?: ZipExtractOptions): ZipMember[];
    /** Raw DEFLATE (RFC 1951, no framing); level 1..12 with gzip's meaning (absent is 6; 10..12 are 9), RangeError outside. Same encoder as deflated ZipPack members. `format: "zlib"` wraps the same body in RFC 1950 framing: header 0x78 then FLEVEL as zlib sets it (level 1 -> 0x01, 2..5 -> 0x5e, 6 -> 0x9c, 7 and up -> 0xda), and the big-endian Adler-32 of the input after the body; "raw" is the default and anything else is a TypeError. */
    function deflate(data: BytesInput, opts?: { level?: number; format?: "raw" | "zlib" }): Uint8Array;
    /** Raw DEFLATE (RFC 1951) decompression; trailing bytes are refused. Output is capped at 1 GiB per call or maxOutputBytes when given. `format: "zlib"` reads RFC 1950 framing: any window size (CINFO 0..7) is accepted; a bad header, a preset dictionary (FDICT, unsupported), a missing or extra trailer byte and an Adler-32 mismatch are each a SyntaxError naming the fault. */
    function inflate(data: ByteView, opts?: { asString?: boolean; maxOutputBytes?: number; format?: "raw" | "zlib" }): Uint8Array | string;

    /** The codec names Compressor accepts. */
    type CompressorAlgo = "gzip" | "lz4" | "lz4frame" | "zstd" | "brotli" | "snappy";
    /** A compiled codec: configuration and codec scratch owned once and reused across calls; prefer it to the one-shots for many buffers. */
    class Compressor implements DynResource {
        /** algo: gzip | lz4 | lz4frame | zstd | brotli | snappy; `level` follows that codec's one-shot bounds and default (gzip 6); `checksum` is lz4frame-only, `dict` lz4-only. */
        constructor(opts: { algo: CompressorAlgo; level?: number; checksum?: boolean; dict?: ByteView });
        /** Compresses one buffer with this object's configuration. */
        compress(data: BytesInput): Uint8Array;
        /** Decompresses one buffer; `maxOutputBytes` lowers the output cap for this call. */
        decompress(data: ByteView, opts?: { asString?: boolean; maxOutputBytes?: number }): Uint8Array | string;
        /** The configured codec name. */
        readonly algo: CompressorAlgo;
        /** CRC-32C of the dictionary, or null when none is set. */
        readonly dictId: number | null;
        close(): void;
        dispose(): void;
        readonly closed: boolean;
        readonly [Symbol.dispose]: () => void;
    }

    /** Aho-Corasick automaton replacing known phrases with codes. */
    class Dictionary implements DynResource {
        /** The automaton is compiled here from non-empty phrases; it pays off when payloads are built from a fixed vocabulary. */
        constructor(phrases: string[]);
        /** Compresses one buffer with this object's configuration. */
        compress(data: BytesInput): Uint8Array;
        /** Decompresses one buffer; `maxOutputBytes` lowers the output cap for this call. */
        decompress(data: ByteView, opts?: { maxOutputBytes?: number }): Uint8Array;
        /** A stable hash of the phrase list. */
        readonly id: number;
        /** The number of phrases. */
        readonly size: number;
        close(): void;
        dispose(): void;
        readonly closed: boolean;
        readonly [Symbol.dispose]: () => void;
    }
}

/** dyna:config — configuration files: TOML 1.0, INI, `.env` (with optional process-environment assignment), front matter. */
declare module "dyna:config" {
    /** TOML 1.0; date-times stay RFC 3339 strings. SyntaxError: a quote run of six or more, or `[[x]]` extending anything but a previous `[[x]]` header. */
    namespace TOML {
        /** Parses a full TOML 1.0 document; key collisions and leading zeros are refused. A leading BOM is skipped and NUL in strings is kept. An integer is a Number, rounded like JSON.parse beyond +-(2^53-1) (owner decision D8: lossy by default); { bigint: true } returns an exact BigInt beyond +-(2^53-1) instead (TOML integers are int64; wider is a SyntaxError). A non-object options value, an unknown key or a non-boolean bigint is a TypeError. */
        function parse(text: string, opts?: { bigint?: boolean }): Record<string, unknown>;
        /** Serializes a plain object root; NaN/Infinity render as nan/inf/-inf, -0 as -0.0. A BigInt in the int64 range is an integer (wider is a TypeError); an integer Number beyond +-(2^53-1) is written as a float so it parses back as the same Number. `null`/`undefined` are a TypeError (TOML 1.0 has no null literal). */
        function stringify(value: unknown): string;
    }
    /** Classic INI reading and writing. */
    namespace INI {
        /** Reads [section] headers, key=value pairs, key[]=v lists, and bare keys as true. */
        function parse(text: string): Record<string, unknown>;
        /** Object to INI: top-level scalars, then one [section] per object value; values parse back as strings; deeper nesting, arrays and null are refused. */
        function stringify(record: Record<string, unknown>): string;
    }
    /** dotenv grammar .env parsing and loading. */
    namespace Env {
        /** Parses KEY=value records; lines without `=` are skipped. In double-quoted values `\n` `\t` `\r` `\0` expand to LF, TAB, CR, NUL and a backslash before any other character is dropped, so stringify() output (NUL included) round-trips. */
        function parse(text: string): Record<string, string>;
        /** Parses a .env file and assigns it into the process environment. Defaults: assign true, override false, expand false; with expand, values expand in file order.
         *  Caps: file 8 MiB; keys are 1..4096 bytes without `=` or NUL; a NUL in a value is a TypeError, an expansion above 1 MiB a RangeError. */
        function load(path: string, opts?: { assign?: boolean; override?: boolean; expand?: boolean }): Record<string, string>;
        /** Record to .env text; values must be strings, and those that cannot sit bare are double-quoted with the escapes parse() expands. */
        function stringify(record: Record<string, string>): string;
    }
    /** Front-matter splitting (YAML/TOML/JSON fences); data stays text. */
    namespace FrontMatter {
        /** Splits at a first-line fence; `data`/`lang` are null when absent. */
        function split(text: string): { data: string | null; body: string; lang: string | null };
    }
}

/** dyna:crypto — AEAD (AES-GCM, ChaCha20-Poly1305), signatures (RSA, ECDSA, Ed25519), key exchange, password hashing, HMAC/KDFs, X.509, JWT (HOTP/TOTP live in dyna:otp).
 *  Secrets compare in constant time. The OpenSSL-backed names (AEAD classes, RSA/ECDSA/ECDH/X509, Ed25519/X25519, Scrypt) exist only when crypto is built with the OpenSSL library (CONFIG_OPENSSL; tools/modules.tsv lists pkg openssl for crypto, with no dependency on the "tls" module): feature-detect via the namespace. */
declare module "dyna:crypto" {
    /** keyed/derive options for the BLAKE one-shots re-exported here. */
    interface Blk2Opts {
        key?: BytesInput;
        length?: number;
    }
    /** BLAKE3: key exactly 32 bytes; {context}/{deriveKey} = derive-key. */
    interface Blk3Opts {
        key?: BytesInput;
        length?: number;
        context?: string;
        deriveKey?: string;
    }
    /** A PEM key pair. */
    interface KeyPair {
        privateKey: string;
        publicKey: string;
    }

    /** OpenBSD $2b$ bcrypt. The password KDFs (Bcrypt.hash/verify, Argon2id.hash/verify, PBKDF2, Scrypt) return Promises: the arguments are checked and the password and salt copied on the calling thread, then the derivation runs as one job on the shared io pool while the event loop keeps serving (with --io-threads 0 it runs inline and the promise is already settled when the call returns). Argument errors (a wrong type, a value out of range, an unknown option key, a malformed PHC string) throw TypeError or RangeError synchronously, before any work is queued; only a failure of the derivation itself (out of memory, a parameter set the engine rejects) rejects the promise. N concurrent calls run up to the pool width at once, each holding its own working memory (Argon2id: `memory` KiB, Scrypt: 128*r*N bytes) until it settles; `dyna.maxConcurrent` (KDFMetaOptions) lowers that for the memory-hard ones. A script that hits --timeout-ms while a derivation runs is stopped within the hard-stop grace (exit 113): the job cannot be interrupted mid-derivation, so exit no longer waits out the 10 s teardown for it; the watchdog caps the drain of cancelled offloaded work at the grace window (tests/sys/test_sys_audit_w2_native_1.js pins it). There is no synchronous form; HKDF, which is cheap, stays synchronous. */
    namespace Bcrypt {
        /** Hashes a password with a fresh salt; rounds 4..31 (else RangeError); a password over bcrypt's 72-byte limit is a RangeError, never truncated. */
        function hash(password: string, rounds?: number): Promise<string>;
        /** Constant-time verify; accepts $2a$/$2b$/$2y$ prefixes; the hash's cost is capped at 20. */
        function verify(password: string, hash: string): Promise<boolean>;
    }

    /** dyna meta configuration of the memory-hard KDFs (Argon2id.hash/verify, Scrypt), an extension beyond the RFCs. A strict bag: an unknown key is a TypeError. `maxConcurrent` (an integer 1..2^31-1, else RangeError) makes this call wait, after its arguments are checked and before any memory is allocated, until fewer than `maxConcurrent` memory-hard derivations of this thread (Argon2id and Scrypt, capped or not) are in flight; waiting calls start in arrival order as slots free. Unset, a call is bounded only by the io pool width. Each Worker counts its own. */
    interface KDFMetaOptions {
        maxConcurrent?: number;
    }

    /** Argon2id v0x13 (RFC 9106). hash() and verify() return Promises and run on the shared io pool (see Bcrypt). */
    namespace Argon2id {
        /** Argon2id options: memory in KiB, salt at least 8 bytes; `encoded` selects hash()'s return form. */
        interface Argon2idOpts {
            iterations?: number;
            memory?: number;
            parallelism?: number;
            hashLen?: number;
            /** Default true: hash() returns the PHC string `$argon2id$v=19$m=..,t=..,p=..$salt$hash`; false returns the raw tag bytes. */
            encoded?: boolean;
            /** dyna meta configuration: see KDFMetaOptions. */
            dyna?: KDFMetaOptions;
        }
        /** PHC string form (default): the salt and every parameter ride in the string, so storing it is the whole contract. */
        function hash(password: BytesInput, salt: ByteView, opts?: Argon2idOpts & { encoded?: true }): Promise<string>;
        /** Raw tag form with { encoded: false }. */
        function hash(password: BytesInput, salt: ByteView, opts?: Argon2idOpts & { encoded: false }): Promise<Uint8Array>;
        /** Verifies a PHC string (untrusted input). Caps: iterations <= 16, parallelism <= 16, memory <= 1048576 (1 GiB) KiB; more is a RangeError. */
        function verify(hashString: string, password: BytesInput, opts?: { dyna?: KDFMetaOptions }): Promise<boolean>;
        /** Raw form: recomputes with the same parameters and compares in constant time. */
        function verify(password: BytesInput, salt: ByteView, expectedHash: ByteView, opts?: Argon2idOpts): Promise<boolean>;
    }

    /** RSA keys and signatures: PKCS#1 v1.5, RSASSA-PSS and RSA-OAEP. */
    namespace RSA {
        /** Defaults to 2048 bits. */
        function generate(bits?: 2048 | 3072 | 4096): KeyPair;
        /** md is "sha1"|"sha256"|"sha384"|"sha512" (case-insensitive). */
        function sign(md: string, privateKey: string, msg: BytesInput): Uint8Array;
        /** True when the signature checks against the public key. */
        function verify(md: string, publicKey: string, msg: BytesInput, sig: ByteView): boolean;
        /** RSASSA-PSS. saltLen: a byte count, "max", or "auto" (default; interoperates with OpenSSL); mgf1Hash defaults to md. */
        function signPSS(md: string, privateKey: string, msg: BytesInput,
                         opts?: { saltLen?: number | "max" | "auto"; mgf1Hash?: string }): Uint8Array;
        /** RSASSA-PSS (RFC 8017): the same digest table and the same 2048-bit key floor as `sign`/`verify`, with the padding that carries a randomized salt. */
        function verifyPSS(md: string, publicKey: string, msg: BytesInput, sig: ByteView,
                           opts?: { saltLen?: number | "max" | "auto"; mgf1Hash?: string }): boolean;
        /** RSA-OAEP: seal to a public key, open with the private one. md defaults to "sha256"; `label` must round-trip exactly; 2048-bit key floor. */
        namespace OAEP {
            function seal(publicKey: string, plaintext: BytesInput,
                          opts?: { md?: string; mgf1Hash?: string; label?: BytesInput }): Uint8Array;
            function open(privateKey: string, sealed: ByteView,
                          opts?: { md?: string; mgf1Hash?: string; label?: BytesInput }): Uint8Array;
        }
    }

    /** X.509 certificate parsing and self-signed generation. */
    namespace X509 {
        /** The parsed fields of a certificate. */
        interface X509Info {
            subject: string;
            issuer: string;
            serialNumber: string;
            version: number;
            /** OpenSSL ASN1_TIME text (e.g. "Sep 15 19:48:26 2026 GMT"). */
            notBefore: string;
            notAfter: string;
            fingerprint: string;
            sans: { dns: string[]; ip: string[]; email: string[] };
        }
        /** Parses a PEM string or DER bytes; malformed input is refused. */
        function parse(cert: BytesInput): X509Info;
        /** Builds a v3 self-signed SHA-256 certificate (days defaults to 365); `subject` is a bare common name, NOT a DN ("/CN=x/O=y" becomes the CN verbatim); `sans` are classified by shape (email, IP, DNS); unsupported extensions are refused by name. With `sans` omitted (and no subjectAltName extension), a hostname- or IP-shaped subject is also the one SAN, since TLS clients match SANs, not the CN; `sans: []` means none. */
        function generateSelfSigned(opts: {
            key: string;
            subject?: string;
            days?: number;
            sans?: string[];
            extensions?: { name: string; value: string; critical?: boolean }[];
        }): string;
    }

    /** ECDSA raw R||S or DER signatures. */
    namespace ECDSA {
        /** The curve defaults to P-256. */
        function generate(curve?: "P-256" | "P-384" | "P-521"): KeyPair;
        /** md "sha1"|"sha256"|"sha384"|"sha512"; format "raw" (default) or "der". */
        function sign(md: string, privateKey: string, msg: BytesInput, opts?: { format?: "raw" | "der" }): Uint8Array;
        /** True for a valid raw or DER signature. A raw signature of the wrong length is a plain false. */
        function verify(md: string, publicKey: string, msg: BytesInput, sig: ByteView, opts?: { format?: "raw" | "der" }): boolean;
    }

    /** ECDH key agreement over X9.63. */
    namespace ECDH {
        /** The curve defaults to P-256. */
        function generate(curve?: "P-256" | "P-384" | "P-521"): KeyPair;
        /** The raw shared secret; a small-order peer point is refused. */
        function derive(privateKey: string, peerPublicKey: string): Uint8Array;
    }

    /** Ed25519 one-shot signatures; raw 32-byte keys. */
    function Ed25519Generate(): { privateKey: Uint8Array; publicKey: Uint8Array };
    /** A 64-byte signature over the whole message. Ed25519 is one-shot by construction. */
    function Ed25519Sign(privateKey: ByteView, message: BytesInput): Uint8Array;
    /** True when the signature is valid. A wrong-size signature returns false, indistinguishable from any other forgery. */
    function Ed25519Verify(publicKey: ByteView, message: BytesInput, signature: ByteView): boolean;

    /** X25519 key agreement; raw 32-byte keys. A small-order (incl. all-zero) peer key is a TypeError. */
    function X25519Generate(): { privateKey: Uint8Array; publicKey: Uint8Array };
    /** The 32-byte shared secret. */
    function X25519Derive(privateKey: ByteView, peerPublicKey: ByteView): Uint8Array;

    /** PEM <-> raw converters between 32-byte raw keys and PKCS#8 / SPKI PEM (byte-exact with openssl genpkey). */
    function Ed25519PemToRaw(pem: string): { privateKey?: Uint8Array; publicKey: Uint8Array };
    /** The bridge between the module's 32-byte raw keys and the PEM world (PKCS#8 `-----BEGIN PRIVATE KEY-----`, SPKI `-----BEGIN PUBLIC KEY-----`). */
    function Ed25519PemFromRaw(keys: { privateKey?: ByteView; publicKey?: ByteView }): { privateKey?: string; publicKey: string };
    function X25519PemToRaw(pem: string): { privateKey?: Uint8Array; publicKey: Uint8Array };
    function X25519PemFromRaw(keys: { privateKey?: ByteView; publicKey?: ByteView }): { privateKey?: string; publicKey: string };

    /** AES-GCM AEAD; key exactly 16/24/32 bytes (else TypeError), nonce exactly 12 bytes (else TypeError). */
    class AESGCM implements DynResource {
        /** Binds the key once. AES-NI / ARMv8 crypto extensions are selected by the backend at runtime. */
        constructor(key: ByteView);
        /** Encrypts and appends the 16-byte tag; nonce must be exactly 12 bytes. */
        seal(nonce: ByteView, plaintext: BytesInput, aad?: BytesInput): Uint8Array;
        /** Decrypts and authenticates; a forged tag throws `authentication failed`. */
        open(nonce: ByteView, sealed: ByteView, aad?: BytesInput): Uint8Array;
        /** Seals into the CALLER's buffer (no allocation), returns bytes written; {tagOut} detaches the 16-byte tag, {prefixNonce:true} emits nonce||ciphertext||tag. */
        sealInto(out: Uint8Array, nonce: ByteView, plaintext: BytesInput,
                 aad?: BytesInput, opts?: { tagOut?: Uint8Array; prefixNonce?: boolean }): number;
        /** The open twin: {tag} reads a detached tag, {prefixNonce:true} reads the leading nonce from the blob; a forged tag throws like open(). */
        openInto(out: Uint8Array, nonce: ByteView | undefined, sealed: ByteView,
                 aad?: BytesInput, opts?: { tag?: Uint8Array; prefixNonce?: boolean }): number;
        /** Seals with a fresh CSPRNG 12-byte nonce and returns it beside ciphertext||tag; random nonces bound one key to ~2^32 messages. */
        sealRandom(plaintext: BytesInput, aad?: BytesInput): { nonce: Uint8Array; sealed: Uint8Array };
        close(): void;
        dispose(): void;
        readonly closed: boolean;
        readonly [Symbol.dispose]: () => void;
    }

    /** ChaCha20-Poly1305 AEAD; key exactly 32 bytes, nonce exactly 12 bytes (else TypeError). */
    class ChaCha20Poly1305 implements DynResource {
        /** `seal`, `open`, `sealInto`, `openInto`, `close`, `dispose` and `closed` behave exactly as `AESGCM`'s, with the same 12-byte nonce, 16-byte tag, and throw-on-forgery rule. */
        constructor(key: ByteView);
        seal(nonce: ByteView, plaintext: BytesInput, aad?: BytesInput): Uint8Array;
        open(nonce: ByteView, sealed: ByteView, aad?: BytesInput): Uint8Array;
        sealInto(out: Uint8Array, nonce: ByteView, plaintext: BytesInput,
                 aad?: BytesInput, opts?: { tagOut?: Uint8Array; prefixNonce?: boolean }): number;
        openInto(out: Uint8Array, nonce: ByteView | undefined, sealed: ByteView,
                 aad?: BytesInput, opts?: { tag?: Uint8Array; prefixNonce?: boolean }): number;
        sealRandom(plaintext: BytesInput, aad?: BytesInput): { nonce: Uint8Array; sealed: Uint8Array };
        close(): void;
        dispose(): void;
        readonly closed: boolean;
        readonly [Symbol.dispose]: () => void;
    }

    /** Streaming HMAC with a derived key schedule reused across calls. */
    class Hmac implements DynResource {
        /** Derives the block-sized key schedule once; every later `sign`/`verify` reuses it. */
        constructor(algorithm: string, key: BytesInput);
        /** A complete MAC; the object is ready for the next message. */
        sign(msg: BytesInput): Uint8Array;
        /** A complete MAC; the object is ready for the next message (finalise resets). */
        signHex(msg: BytesInput): string;
        /** Streaming absorb; returns this. */
        update(msg: BytesInput): this;
        /** Finishes the accumulated stream. */
        digest(): Uint8Array;
        /** Finish the accumulated stream. */
        digestHex(): string;
        /** Returns the HMAC to its initial state, discarding any update() prefix; the key schedule is kept. */
        reset(): void;
        /** Constant-time compare; tag may be raw bytes or a hex string. */
        verify(msg: BytesInput, tag: BytesInput): boolean;
        readonly algorithm: string;
        readonly digestSize: number;
        close(): void;
        dispose(): void;
        readonly closed: boolean;
        readonly [Symbol.dispose]: () => void;
    }

    /** One-shot HMAC. */
    function HMAC(algorithm: string, key: BytesInput, data: BytesInput): Uint8Array;
    /** The one-shot HMAC; `HMACHex` returns the same digest as lowercase hex. */
    function HMACHex(algorithm: string, key: BytesInput, data: BytesInput): string;

    /** The unkeyed digests and checksums of dyna:hash, re-exported (same functions) so PKCE challenges and fingerprints need no second import. */
    function MD5(data: BytesInput): Uint8Array;
    function MD5Hex(data: BytesInput): string;
    function SHA1(data: BytesInput): Uint8Array;
    function SHA1Hex(data: BytesInput): string;
    function SHA224(data: BytesInput): Uint8Array;
    function SHA224Hex(data: BytesInput): string;
    function SHA256(data: BytesInput): Uint8Array;
    function SHA256Hex(data: BytesInput): string;
    function SHA384(data: BytesInput): Uint8Array;
    function SHA384Hex(data: BytesInput): string;
    function SHA512(data: BytesInput): Uint8Array;
    function SHA512Hex(data: BytesInput): string;
    function CRC32(data: BytesInput): number;
    function CRC32C(data: BytesInput): number;
    /** Same as dyna:hash Adler32 / CDCChunks (dyna:crypto re-exports the hash table). */
    function Adler32(data: BytesInput): number;
    function CDCChunks(data: BytesInput, opts?: { min?: number; avg?: number; max?: number }): Uint32Array;
    function XXHash32(data: BytesInput, seed?: number): number;
    function XXHash64(data: BytesInput, seed?: number, opts?: { as?: "hex" | "bigint" | "bytes" }): string | bigint | Uint8Array;
    /** XXH3-64 (default secret); same shapes as XXHash64. Not a security primitive. */
    function XXH3_64(data: BytesInput, seed?: number, opts?: { as?: "hex" | "bigint" | "bytes" }): string | bigint | Uint8Array;
    function SHA3_224(data: BytesInput): Uint8Array;
    function SHA3_224Hex(data: BytesInput): string;
    function SHA3_256(data: BytesInput): Uint8Array;
    function SHA3_256Hex(data: BytesInput): string;
    function SHA3_384(data: BytesInput): Uint8Array;
    function SHA3_384Hex(data: BytesInput): string;
    function SHA3_512(data: BytesInput): Uint8Array;
    function SHA3_512Hex(data: BytesInput): string;
    function Keccak256(data: BytesInput): Uint8Array;
    function Keccak256Hex(data: BytesInput): string;
    function SHAKE128(data: BytesInput, length?: number): Uint8Array;
    function SHAKE128Hex(data: BytesInput, length?: number): string;
    function SHAKE256(data: BytesInput, length?: number): Uint8Array;
    function SHAKE256Hex(data: BytesInput, length?: number): string;
    /** Keyed/derive forms take an options object second: BLAKE2 keys are 0..64 (b) / 0..32 (s) bytes, BLAKE3 keys exactly 32; {context} derives a 32-byte key. */
    function BLAKE3(data: BytesInput, length?: number, opts?: never): Uint8Array;
    function BLAKE3(data: BytesInput, opts?: Blk3Opts): Uint8Array;
    function BLAKE3Hex(data: BytesInput, length?: number, opts?: never): string;
    function BLAKE3Hex(data: BytesInput, opts?: Blk3Opts): string;
    function BLAKE2b(data: BytesInput, length?: number, opts?: never): Uint8Array;
    function BLAKE2b(data: BytesInput, opts?: Blk2Opts): Uint8Array;
    function BLAKE2bHex(data: BytesInput, length?: number, opts?: never): string;
    function BLAKE2bHex(data: BytesInput, opts?: Blk2Opts): string;
    function BLAKE2s(data: BytesInput, length?: number, opts?: never): Uint8Array;
    function BLAKE2s(data: BytesInput, opts?: Blk2Opts): Uint8Array;
    function BLAKE2sHex(data: BytesInput, length?: number, opts?: never): string;
    function BLAKE2sHex(data: BytesInput, opts?: Blk2Opts): string;
    function Murmur3_128(data: BytesInput, seed?: number): Uint8Array;
    function Murmur3_128Hex(data: BytesInput, seed?: number): string;

    /** RFC 5869 HKDF extract-and-expand. Defaults: hash "sha256", length 32; an absent `salt` is the all-zero salt of hash length. */
    function HKDF(opts: { hash?: string; key: BytesInput; salt?: BytesInput; info?: BytesInput; length?: number }): Uint8Array;

    /** RFC 8018 PBKDF2: hash "sha256", iterations 100000, length 32 by default; `salt` is REQUIRED (TypeError); iterations x output blocks capped at 2^24. Returns a Promise and runs on the shared io pool (see Bcrypt). */
    function PBKDF2(opts: { hash?: string; password: BytesInput; salt: BytesInput; iterations?: number; length?: number }): Promise<Uint8Array>;

    /** RFC 7914 scrypt; budgets (RangeError): memory 128*r*(N+p+2) <= 1 GiB, work N*r*p <= 2^26. Returns a Promise and runs on the shared io pool (see Bcrypt). */
    function Scrypt(password: BytesInput, salt: BytesInput, opts?: { N?: number; r?: number; p?: number; keyLen?: number; dyna?: KDFMetaOptions }): Promise<Uint8Array>;

    /** OS-entropy CSPRNG bytes: the door for security material; use dyna:random's seeded Random for reproducible or fast simulation. */
    function RandomBytes(count?: number): Uint8Array;

    /** Constant-time comparison; different lengths return false (oauth2.secureCompare is the string-friendly form).
     *  Module rule: key/secret arguments must be a string, ArrayBuffer or byte view and are never stringified; anything else is a TypeError. */
    function TimingSafeEqual(a: ByteView, b: ByteView): boolean;

    /** JWS signing; HS/RS/ES algorithms. */
    function JWTSign(payload: unknown, key: BytesInput, opts?: { alg?: string }): string;

    /** Verifies a JWT: signature, header alg in the required `algorithms` allowlist (`none` always refused; every entry must be a string, wherever the match is), key family matching the alg; `crit` headers refused.
     *  exp/nbf/aud/iss are NOT validated here: use dyna:oauth2's verifyJWT for claims. Pass T to type the payload. */
    function JWTVerify<T = unknown>(token: string, key: BytesInput, opts: { algorithms: string[] }): T;
}

/** dyna:csv — RFC 4180 CSV: in-memory parse/stringify, streaming batches, and CSVFile, a file-backed table.
 *  Cells are VERBATIM: a leading = + - @ can run as a spreadsheet formula; escape it or use DataFrame TO_CSV {escapeFormulas:true}. */
declare module "dyna:csv" {
    /** Parses CSV text in memory; a leading BOM is stripped and empty text is the empty table.
     *  Rows are shaped to the first row's width (short rows pad with "", long rows truncate); rows in syntax errors are 1-based. */
    function parse(text: string, opts?: {
        /** Field separator; exactly one ASCII char, not `"` / CR / LF, != quote. */
        delimiter?: string;
        /** Quote char; exactly one ASCII char, not `,` / CR / LF, != delimiter. */
        quote?: string;
        /** Default true; false parses headerless text: `headers` is [] and every row is data. */
        hasHeader?: boolean;
        /** Default false (tolerant); true throws a SyntaxError naming the row on garbage after a closing quote or an unterminated quote. */
        strict?: boolean;
    }): { headers: string[]; rows: string[][]; totalRows: number };
    /** Serializes rows to CSV text, every line ending in LF. string[][] rows are written verbatim (the header is the caller's first row).
     *  Record rows take their columns from the FIRST row's keys and get a header line unless hasHeader:false; mixing the two forms is refused; an undefined cell or a hole is an empty field, null prints as null, and output past the maximum string length is a RangeError. */
    function stringify(rows: string[][] | Record<string, unknown>[], opts?: {
        delimiter?: string;
        quote?: string;
        /** Default true; false suppresses the derived header of object rows. */
        hasHeader?: boolean;
    }): string;
    /** Options of parseDataFrame and CSVFile#toDataFrame. The first record is the header (names must be unique); a BOM is stripped; a blank line is skipped when the header has 2+ columns. */
    interface DataFrameReadOptions {
        /** Columns to keep, in this order (default: every column, file order); an unknown or repeated name is a TypeError. */
        columns?: string[];
        /** Forces a column's type. Untyped columns are inferred: "f64" when every non-empty cell is a decimal number ([+-]digits[.digits][e[+-]digits], parsed exactly as Number() would; inf, nan, hex and padded cells are text), else "str". An empty cell is NaN in an f64 column and "" in a str column; a forced f64 cell that is not a number, and any i32 cell that is not an integer in int32 range (empty included), is a TypeError naming the row, column and cell. */
        dtypes?: Record<string, "f64" | "i32" | "str">;
        /** Data rows to skip after the header (default 0). */
        offset?: number;
        /** Data rows to keep (default all). */
        limit?: number;
        delimiter?: string;
        quote?: string;
        /** Default false: a short row pads with empty cells, a long row drops its extra cells, text after a closing quote joins the cell, an unterminated quote runs to the end. true makes each of these a SyntaxError naming the record (rows count records from 1 at the header). */
        strict?: boolean;
    }
    /** CSV text straight into a DataFrame in one native pass: numeric columns become Float64Array/Int32Array, text columns dictionary-encoded strings, with no JS string per cell. Empty text is a TypeError (no header); a header-only text is a 0-row frame whose untyped columns are f64.
     *  Regular text (every record the header's width, regular quoting) is read through the structural index of index(), on several threads from 2 MiB; other text through the original scalar reader. Both give the same frame and the same errors. */
    function parseDataFrame(text: string, opts?: DataFrameReadOptions): import("dyna:dataframe").DataFrame;
    /** A structural index over CSV bytes, from index(). Cells are read straight from the source bytes on access; no string exists until one is asked for.
     *  Rows follow parse(): the first record fixes the width (short rows read as "", extra cells are dropped) and, with hasHeader (default true), is the header.
     *  Lifetime: the index pins its source. A string source is held (an ASCII string is read in place); Uint8Array and ArrayBuffer sources are copied at index() time, so later writes to them are not seen; a Path is memory-mapped read-only until the index is collected, and the file must not be truncated meanwhile. */
    interface CsvIndex {
        /** Header cells, or [] with hasHeader:false. */
        readonly headers: string[];
        /** Data rows (the header row excluded). */
        readonly rowCount: number;
        /** The width fixed by the first record. */
        readonly columnCount: number;
        /** One cell; `column` is an index or a header name (TypeError when unknown); a row outside 0..rowCount-1 is a RangeError. */
        get(row: number, column: number | string): string;
        /** One row, padded or truncated to columnCount. */
        row(row: number): string[];
        /** One whole column. No dtype: string[]. "f64": Float64Array parsed exactly as Number() would (decimal text only, an empty cell is NaN). "i32" / "i64": Int32Array / BigInt64Array of decimal integers.
         *  "dict": {codes, values}, values holding each distinct cell once in first-appearance order and codes[i] indexing it, so a group-by needs no per-row string.
         *  A cell that does not fit the dtype is a TypeError naming its row, column and text. Columns of 128 Ki rows or more are converted on several threads. */
        column(column: number | string): string[];
        column(column: number | string, dtype: "f64"): Float64Array;
        column(column: number | string, dtype: "i32"): Int32Array;
        column(column: number | string, dtype: "i64"): BigInt64Array;
        column(column: number | string, dtype: "dict"): { codes: Int32Array; values: string[] };
    }
    /** Indexes CSV text, bytes or a file without creating a string per cell: field boundaries go into one flat offset array, built with SIMD block classification and, for inputs of 2 MiB or more, on several threads.
     *  The dialect, BOM stripping and tolerant/strict rules are parse()'s; strict:true throws the same SyntaxError. Sources up to 4 GiB (RangeError past that). A Path source is memory-mapped: truncating the file from another process while a read or edit is running can terminate the process with SIGBUS; copy the file first if other writers may shrink it. */
    function index(source: string | Uint8Array | ArrayBuffer | import("dyna:file").Path, opts?: {
        delimiter?: string;
        quote?: string;
        /** Default true. */
        hasHeader?: boolean;
        /** Default false. */
        strict?: boolean;
    }): CsvIndex;
    /** A file-backed table: each method load-modify-stores the bound file; writes are atomic whole-file replacements. A Path source is memory-mapped: truncating the file from another process while a read or edit is running can terminate the process with SIGBUS; copy the file first if other writers may shrink it. */
    class CSVFile implements DynResource {
        /** Binds the path; touches no disk until a method runs. */
        constructor(path: import("dyna:file").Path);
        /** Creates the file from headers and optional rows; every header must be a string (a hole included, TypeError naming the index). */
        create(opts: { headers: string[]; rows?: (string | number)[][]; overwrite?: boolean }): { path: import("dyna:file").Path; rows: number };
        /** Loads the file; options offset/limit/columns (columns: strings only, like create's headers). delimiter/quote change the input dialect (read-only support: mutators rewrite the canonical comma/quote pair). */
        read(opts?: { offset?: number; limit?: number; columns?: string[]; strict?: boolean; delimiter?: string; quote?: string }): { headers: string[]; rows: string[][]; totalRows: number };
        /** Appends rows ({rows}: positional arrays or objects keyed by header); a bare array is ONE positional row, truncated to the header's width (only those cells are read). */
        addRow(opts: { rows: (string | number)[][] | Record<string, unknown>[]; strict?: boolean; durable?: boolean } | (string | number)[]): { added: number; totalRows: number };
        /** Sets one cell. */
        updateCell(opts: { row: number; column?: string; columnIndex?: number; value: string; strict?: boolean; durable?: boolean }): { row: number; column: string; value: string };
        /** Removes the data row at `row`; `removed` echoes the removed row's index. */
        removeRow(opts: { row: number; strict?: boolean; durable?: boolean }): { removed: number; totalRows: number };
        /** Appends a column, filling rows with defaultValue. */
        addColumn(opts: { column: string; defaultValue?: string; strict?: boolean; durable?: boolean }): { column: string; totalColumns: number };
        /** Drops the named or indexed column. */
        removeColumn(opts: { column?: string; columnIndex?: number; strict?: boolean; durable?: boolean }): { removedIndex: number; totalColumns: number };
        /** Renames a column; a no-op when names match. */
        renameColumn(opts: { oldName: string; newName: string; strict?: boolean; durable?: boolean }): { oldName: string; newName: string };
        /** One column's values over a [start, end) window -- or its {offset, limit} alias (never both forms); the default 1000-row window cap is raised with maxRows. */
        readColumnValuesRange(opts: { column: string; start?: number; end?: number; offset?: number; limit?: number; maxRows?: number; strict?: boolean }): (string | number)[];
        /** Rows [start, end) as arrays; default is a single row. Accepts {offset, limit} instead of {start, end} (never both); the default 100-row cap is raised with maxRows. */
        readRowRange(opts?: { start?: number; end?: number; offset?: number; limit?: number; maxRows?: number; strict?: boolean }): { headers: string[]; rows: string[][] };
        /** Projects columns over a [start, end) window, capped at 100 rows ({offset, limit} alias accepted, never both forms; maxRows raises the cap). */
        selectColumnRange(opts: { columns: string[]; start?: number; end?: number; offset?: number; limit?: number; maxRows?: number; strict?: boolean }): { columns: string[]; rows: string[][] };
        /** Async-iterable batches {headers, rows, offset, totalRows} of at most `batch` rows (default 1000, max 100000); the file is parsed once, at the first next(), and later batches are cut from that snapshot (a change to the file mid-iteration is not seen). */
        rows(opts?: { batch?: number; delimiter?: string; quote?: string; strict?: boolean }): AsyncIterableIterator<{ headers: string[]; rows: string[][]; offset: number; totalRows: number }>;
        /** Reads the file into a DataFrame with parseDataFrame's options and rules. */
        toDataFrame(opts?: DataFrameReadOptions): import("dyna:dataframe").DataFrame;
        close(): void;
        dispose(): void;
        readonly closed: boolean;
        readonly [Symbol.dispose]: () => void;
    }
}

/** dyna:decimal — exact decimal arithmetic (IEEE 754-2008 decimal128, 34 digits by default) and Money in integer minor units. */
declare module "dyna:decimal" {
    // Rounding defaults differ on purpose: round() is halfEven (IEEE), toFixed() is halfUp (invoices); pass a RoundingMode when it matters.

    /** `precision` is 1..5000 significant digits (default 34); exact ops (add/sub/mul/mod/divmod) ignore it; ln/log10/exp throw past a fixed work budget. */
    interface DecimalOptions {
        precision?: number;
        rounding?: RoundingMode | string;
    }
    /** An exact decimal value; methods return new Decimals and accept Decimal, string or number operands (strings avoid binary-float input error). */
    class Decimal {
        /** value: decimal string, JS number (through its shortest round-trip text), or another Decimal. */
        constructor(value: string | number | Decimal);
        /** Exact addition — addition/multiplication are EXACT here, so `opts` is accepted and ignored on add/sub/mul/mod (only div rounds). */
        add(x: Decimal | string | number, opts?: DecimalOptions): Decimal;
        sub(x: Decimal | string | number, opts?: DecimalOptions): Decimal;
        mul(x: Decimal | string | number, opts?: DecimalOptions): Decimal;
        /** The only arithmetic that rounds; division by zero throws. */
        div(x: Decimal | string | number, opts?: DecimalOptions): Decimal;
        /** Exact truncated remainder: the sign follows the dividend (like JS %), mod by zero throws, opts ignored. */
        mod(x: Decimal | string | number, opts?: DecimalOptions): Decimal;
        /** Integer exponentiation; exponent in -10000..10000. */
        pow(n: number, opts?: DecimalOptions): Decimal;
        /** Square root, correctly rounded half-even (`rounding` ignored); a negative value throws RangeError. */
        sqrt(opts?: DecimalOptions): Decimal;
        /** e^x, correctly rounded half-even; RangeError past Emax = 999999; results below the subnormal range are 0. */
        exp(opts?: DecimalOptions): Decimal;
        /** Natural log, correctly rounded half-even; zero or negative throws RangeError. */
        ln(opts?: DecimalOptions): Decimal;
        /** Base-10 log, correctly rounded half-even and exact for powers of ten; zero or negative throws RangeError. */
        log10(opts?: DecimalOptions): Decimal;
        /** The integer toward -Infinity. Exact; no rounding context. */
        floor(): Decimal;
        /** The integer toward +Infinity. Exact; no rounding context. */
        ceil(): Decimal;
        /** The integer toward zero. Exact; no rounding context. */
        trunc(): Decimal;
        /** Exact truncated-division pair [q, r]: q toward zero, r with the dividend's sign, a == q*b + r; division by zero throws. */
        divmod(x: Decimal | string | number, opts?: DecimalOptions): [Decimal, Decimal];
        /** Exact: an integral value in [-2^63, 2^63) as a bigint; a fractional part (no truncation) or a value outside that range throws RangeError. */
        toBigInt(): bigint;
        /** The value zero. The constants are ordinary Decimals. */
        static readonly ZERO: Decimal;
        static readonly ONE: Decimal;
        static readonly TWO: Decimal;
        static readonly TEN: Decimal;
        static readonly NEG_ONE: Decimal;
        /** The magnitude. */
        abs(): Decimal;
        /** The negation; -0 is 0. */
        neg(): Decimal;
        /** -1, 0, or 1. The short legacy form of compare. */
        cmp(x: Decimal | string | number): number;
        /** True when the values are equal (1.5 equals 1.50). */
        equals(x: Decimal | string | number): boolean;
        /** A new Decimal rounded to dp decimal places; dp in -1000..1000; default mode halfEven. */
        round(dp?: number, rounding?: RoundingMode | string): Decimal;
        /** A string with exactly dp digits after the point; default mode halfUp; a negative value that rounds to zero still prints "-0.00". */
        toFixed(dp?: number, rounding?: RoundingMode | string): string;
        /** The exact decimal text. */
        toString(): string;
        toJSON(): string;
        /** The one place a Decimal may become approximate: ToNumber over the exact text. */
        toNumber(): number;
        isZero(): boolean;
        /** -1, 0, or 1. */
        sign(): number;
        /** The number of significant digits. */
        digits(): number;
    }

    /** Integral money: an integer count of minor units plus a 3-letter currency tag. */
    class Money {
        /** `new Money(1999, "USD")` is $19.99: integer arithmetic with a unit, not float-shaped arithmetic. minorDigits defaults to 2;
         *  minorUnits and minorDigits must be integers, the code is upper-cased, and opts must be an object when given. */
        constructor(minorUnits: number, currency: string, opts?: { minorDigits?: number });
        /** Parses the human form ("-1,234.56") digit by digit, never through a double; a value FINER than the minor unit is refused, not rounded ("1.500" ok, "1.005" throws). */
        static fromString(str: string, currency: string, opts?: { minorDigits?: number }): Money;
        /** From an exact Decimal; `d * 10^minorDigits` must be an integer or it is refused, not rounded. */
        static fromDecimal(d: Decimal, currency: string, opts?: { minorDigits?: number }): Money;
        /** From a raw minor-unit count: an integer number, a bigint, or an amount string that is whole minor units ("5.0" ok, "5.5" throws). */
        static fromMinor(n: number | bigint | string, currency: string, opts?: { minorDigits?: number }): Money;
        /** Both operands must share the currency code. */
        add(x: Money): Money;
        sub(x: Money): Money;
        cmp(x: Money): number;
        equals(x: Money): boolean;
        /** Scales by an integer; a fractional multiplier throws. */
        mul(n: number): Money;
        /** Splits into shares whose sum is exactly the original. */
        allocate(shares: number[]): Money[];
        /** The decimal amount with the currency's minor digits. */
        toString(): string;
        toJSON(): string;
        /** The minor-unit integer as a Number: exact through 2^53; use toString()/toDecimal() beyond. */
        amount(): number;
        /** The 3-letter code. */
        currency(): string;
        /** "$19.99" for major currencies, "19.99 USD" otherwise. */
        format(): string;
        /** The amount as an exact Decimal. */
        toDecimal(): Decimal;
    }
}

/** dyna:otp — one-time passwords: RFC 4226 HOTP and RFC 6238 TOTP (moved out of dyna:crypto, which no longer exports them),
 *  otpauth:// key URIs (Google Authenticator Key URI format), enrolment with a QR code (dyna:encoding QREncode), look-ahead and
 *  drift windows, replay protection through the last used counter, and a stateless throttle for failed attempts.
 *  Secrets are raw bytes: a string is taken as its UTF-8 bytes (RFC 4226 Appendix D uses the ASCII seed "12345678901234567890"),
 *  never base32-decoded; KeyURIParse returns the decoded bytes. Codes are compared in constant time and every counter of a
 *  window is computed and compared, so the time spent does not depend on which counter matched. */
declare module "dyna:otp" {
    type OTPAlgorithm = "sha1" | "sha256" | "sha512";

    /** RFC 4226 HOTP: HMAC over the 8-byte big-endian counter, dynamic truncation, modulo 10^digits. digits 6..8 (default 6), algo default "sha1". */
    function HOTPGenerate(secret: BytesInput, counter: number, opts?: { digits?: number; algo?: OTPAlgorithm }): string;

    /** RFC 6238 TOTP: HOTP with counter floor(atSec / period) (T0 = 0). atSec defaults to the current Unix time (pass it for reproducible results); period default 30. */
    function TOTPGenerate(secret: BytesInput, opts?: { atSec?: number; period?: number; digits?: number; algo?: OTPAlgorithm }): string;

    /** HOTP verification of exactly one counter in constant time; a malformed code is FALSE like a wrong guess, never a throw. For a look-ahead window or the matched counter use HOTPMatch. */
    function HOTPVerify(secret: BytesInput, counter: number, code: string, opts?: { digits?: number; algo?: OTPAlgorithm }): boolean;

    /** TOTP verification; window (default 1, at most 4096 periods: RFC 6238 section 5.2 recommends one step of network delay) accepts counters within +/- window of floor(atSec / period); atSec defaults to the current Unix time. It has no replay protection: use TOTPMatch with lastCounter. */
    function TOTPVerify(secret: BytesInput, code: string, opts?: { atSec?: number; period?: number; digits?: number; algo?: OTPAlgorithm; window?: number }): boolean;

    /**
     * RFC 4226 section 7.4 resynchronisation: tries counters counter .. counter + lookAhead (lookAhead an integer in [0, 4096],
     * default 0) and returns the lowest matching counter with next = counter + 1, the value to store as the next expected
     * counter (a code is then never accepted twice), or null. counter is an integer in [0, 2^53-1]. A malformed code is null.
     */
    function HOTPMatch(secret: BytesInput, counter: number, code: string, opts?: { digits?: number; algo?: OTPAlgorithm; lookAhead?: number }): { counter: number; next: number } | null;

    /**
     * RFC 6238 verification with drift and replay protection. With step s = floor(atSec / period) (atSec defaults to the
     * current Unix time), it tries counters s + drift - window .. s + drift + window (window an integer in [0, 4096], default 0;
     * drift an integer, default 0), skips counters at or below lastCounter (RFC 6238 section 5.2: an accepted OTP is never
     * accepted again) and returns { counter, drift: counter - s } or null. When several counters of the window produce the code,
     * the one nearest s + drift wins, the lower on a tie. Store counter as the next lastCounter and drift as the next drift
     * (section 6 resynchronisation). A malformed code is null.
     */
    function TOTPMatch(secret: BytesInput, code: string, opts?: { atSec?: number; period?: number; digits?: number; algo?: OTPAlgorithm; window?: number; drift?: number; lastCounter?: number }): { counter: number; drift: number } | null;

    /** A key from an otpauth:// URI. issuer is absent when the URI has neither an issuer parameter nor a label prefix; period is set for totp, counter for hotp. */
    interface OTPKey {
        type: "totp" | "hotp";
        issuer?: string;
        account: string;
        secret: Uint8Array;
        algo: OTPAlgorithm;
        digits: number;
        period?: number;
        counter?: number;
    }

    /**
     * Parses otpauth://TYPE/LABEL?PARAMETERS (Google Authenticator Key URI format). The scheme, the type (totp or hotp),
     * the algorithm value and the base32 secret are case-insensitive. LABEL is percent-decoded and is "account" or
     * "issuer:account" (a literal or %3A colon; spaces after it are dropped; more than one colon is refused). Parameter
     * values are percent-decoded with "+" read as a space. secret is required (RFC 4648 base32, padding optional, any
     * length of at least one byte; trailing bits must be zero). issuer must equal the label prefix when both are present.
     * algorithm SHA1 (default), SHA256 or SHA512; digits 6 (default) or 8; counter is required for hotp, an integer in
     * [0, 2^53-1]; period (totp, default 30) an integer >= 1. Unknown parameters are ignored, a repeated known parameter is
     * refused. Decoded text must be valid UTF-8 without control characters. @throws {TypeError} when uri is not a string.
     * @throws {SyntaxError} on any other violation.
     */
    function KeyURIParse(uri: string): OTPKey;

    /** Key fields for KeyURIBuild and Enroll. type default "totp"; account (required) and issuer are non-empty text without a colon; digits 6 or 8; period only with totp (default 30), counter only with hotp (default 0). */
    interface OTPKeyInput {
        type?: "totp" | "hotp";
        account: string;
        issuer?: string;
        algo?: OTPAlgorithm;
        digits?: number;
        period?: number;
        counter?: number;
    }

    /**
     * Builds otpauth://TYPE/[issuer:]account?secret=...&issuer=...&algorithm=...&digits=...&period=... (or &counter=...). Every
     * byte of the label and issuer outside A-Z a-z 0-9 - . _ ~ is percent-encoded (a space is %20); the secret is unpadded
     * upper-case base32 of the given bytes; algorithm, digits and period/counter are always written. KeyURIParse(KeyURIBuild(k))
     * gives back k. @throws {TypeError} for a missing or mistyped field or an unknown key. @throws {RangeError} for an empty
     * secret, digits 7, or a name that is empty or holds a colon or control characters.
     */
    function KeyURIBuild(key: OTPKeyInput & { secret: BytesInput }): string;

    /** Enrolment options: the key fields, a secret of 16..64 bytes or secretBytes (16..64, default 20 random bytes from the OS CSPRNG; RFC 4226 asks for at least 128 bits and recommends 160), and QR options for dyna:encoding QREncode. */
    interface OTPEnrollOptions extends OTPKeyInput {
        secret?: BytesInput;
        secretBytes?: number;
        qr?: import("dyna:encoding").QROptions;
        /** dyna-specific meta configuration. qr: false skips both QR renderings (the costly part; uri and secret only). */
        dyna?: { qr?: boolean };
    }

    /**
     * Creates a key for a user: { uri, secret, secretBase32, qr, qrText }. qr is exactly QREncode(uri, opts.qr) and qrText
     * exactly QRToString(uri, opts.qr). Store secret server-side (sealed); show the QR code or secretBase32 to the user once.
     * @throws {TypeError} when both secret and secretBytes are given, a field is mistyped or a key is unknown, or when the build lacks dyna:encoding and dyna.qr is not false.
     */
    function Enroll(opts: OTPEnrollOptions): { uri: string; secret: Uint8Array; secretBase32: string; qr?: { version: number; size: number; modules: Uint8Array }; qrText?: string };

    /**
     * RFC 4226 section 7.3 throttling over caller-held state (no table is kept here): failures is the count of consecutive
     * failed attempts and lastFailureSec the Unix time of the last one (required when failures > 0). The next attempt is
     * allowed from lastFailureSec + stepSec * failures (stepSec an integer in [0, 86400], default 5: a linearly growing delay);
     * at lockoutAfter failures (default 10; 0 disables the lockout) the key is locked until the caller resets it, and
     * retryAtSec is Infinity. atSec defaults to the current Unix time. Reset failures to 0 after a successful match.
     */
    function Throttle(failures: number, lastFailureSec?: number, opts?: { atSec?: number; stepSec?: number; lockoutAfter?: number }): { allowed: boolean; locked: boolean; retryAtSec: number };
}

/** dyna:encoding — byte<->text codecs (hex, base64/32/58/85, varints), charset detection, JSON5, stable JSON, JSONPath, QR codes.
 *  These are BYTE codecs; the global atob/btoa are latin-1 STRING codecs and give different answers, so never mix the two families. */
declare module "dyna:encoding" {

    /** Lowercase hex string, SIMD-accelerated. */
    function HexEncode(data: BytesInput): string;
    /** Hex text into a caller buffer; returns the 2*n bytes written; `out` needs 2*n bytes (RangeError, nothing written); overlap refuses. */
    function hexEncodeInto(data: BytesInput, out: Uint8Array): number;
    /** Returns a Uint8Array; throws SyntaxError on odd length or an invalid digit. */
    function HexDecode(text: string): Uint8Array;
    /** Decodes hex into a caller buffer; returns bytes written; `out` needs text.length/2 bytes or RangeError. */
    function hexDecodeInto(text: string, out: Uint8Array): number;
    /** RFC 4648 base64 (`+/`, padded) over BYTES: Base64Encode(utf8 "é") is "w6k=" while the latin-1 btoa("é") is "6Q==". */
    function Base64Encode(data: BytesInput): string;
    /** Base64 text into a caller buffer; returns the byte count; `out` needs 4*ceil(n/3) bytes or RangeError. */
    function base64EncodeInto(data: BytesInput, out: Uint8Array): number;
    /** Byte-level RFC 4648 decode (not interchangeable with atob output). */
    function Base64Decode(text: string): Uint8Array;
    /** Decodes base64 into a caller buffer; returns the byte count; `out` needs 3*(text.length/4) bytes or RangeError. */
    function base64DecodeInto(text: string, out: Uint8Array): number;
    /** RFC 4648 section 5 base64url (no padding). */
    function Base64URLEncode(data: BytesInput): string;
    /** Unpadded base64url text into a caller buffer; `out` needs 4*ceil(n/3) bytes or RangeError (up to 3 bytes past the count are scratch). */
    function base64UrlEncodeInto(data: BytesInput, out: Uint8Array): number;
    /** A length of `4k+1` throws `SyntaxError` (no byte count encodes to that many characters), and a stray `+`/`/` is rejected. */
    function Base64URLDecode(text: string): Uint8Array;
    /** Decodes base64url into a caller buffer; `out` needs 3*((text.length+3)/4) bytes or RangeError. */
    function base64UrlDecodeInto(text: string, out: Uint8Array): number;
    /** RFC 4648 base32, `=` padded. */
    function Base32Encode(data: BytesInput): string;
    /** Base32 text into a caller buffer; `out` needs ((n+4)/5)*8 bytes or RangeError. */
    function base32EncodeInto(data: BytesInput, out: Uint8Array): number;
    /** Throws `SyntaxError` on invalid input. */
    function Base32Decode(text: string): Uint8Array;
    /** Decodes padded base32 (length % 8 == 0) into a caller buffer; `out` needs (text.length/8)*5 bytes or RangeError. */
    function base32DecodeInto(text: string, out: Uint8Array): number;
    /** Extended-hex base32. */
    function Base32HexEncode(data: BytesInput): string;
    /** Base32hex text into a caller buffer; capacity as base32EncodeInto. */
    function base32HexEncodeInto(data: BytesInput, out: Uint8Array): number;
    function Base32HexDecode(text: string): Uint8Array;
    /** base32hex into a caller-owned buffer; capacity as base32DecodeInto. */
    function base32HexDecodeInto(text: string, out: Uint8Array): number;
    /** Adobe-less ascii85 with the `z` shorthand. */
    function Base85Encode(data: BytesInput): string;
    /** Ascii85 text into a caller buffer; `out` needs ((n+3)/4)*5 bytes or RangeError. */
    function base85EncodeInto(data: BytesInput, out: Uint8Array): number;
    /** Skips whitespace (space/tab/CR/LF/VT/FF) for line-wrapped formats. */
    function Base85Decode(text: string): Uint8Array;
    /** Decodes ascii85 into a caller buffer; `out` needs 4*text.length bytes or RangeError. */
    function base85DecodeInto(text: string, out: Uint8Array): number;
    /** Bitcoin base58; leading zero bytes become leading `1`s. */
    function Base58Encode(data: BytesInput): string;
    /** Base58 text into a caller buffer, zero-allocation; `out` needs (n*8)/5+1 bytes; input over 4096 bytes refuses (the codec is quadratic). */
    function base58EncodeInto(data: BytesInput, out: Uint8Array): number;
    /** Throws `SyntaxError` on a character outside the alphabet. */
    function Base58Decode(text: string): Uint8Array;
    /** Decodes base58 into a caller buffer; `out` needs text.length bytes or RangeError. */
    function base58DecodeInto(text: string, out: Uint8Array): number;
    /** Base58 with a double-SHA256 checksum appended. */
    function Base58CheckEncode(data: BytesInput): string;
    /** Base58check text into a caller buffer; `out` needs ((n+4)*8)/5+1 bytes or RangeError. */
    function base58CheckEncodeInto(data: BytesInput, out: Uint8Array): number;
    /** Throws `SyntaxError` when the checksum does not match or the input is too short to carry one. */
    function Base58CheckDecode(text: string): Uint8Array;
    /** base58check into a caller-owned buffer; capacity as base58DecodeInto. */
    function base58CheckDecodeInto(text: string, out: Uint8Array): number;
    /** Encode in a caller-supplied alphabet of 2..255 distinct characters. */
    function BaseXEncode(data: BytesInput, alphabet: string): string;
    /** Decodes text written in a custom `alphabet`. */
    function BaseXDecode(text: string, alphabet: string): Uint8Array;
    /** LEB128 varint encoding of a non-negative value. */
    function PutUvarint(value: number | bigint): Uint8Array;
    /** Zigzag-encoded signed LEB128. */
    function PutVarint(value: number | bigint): Uint8Array;
    /** Decodes; returns [value, bytesRead]; magnitude is bigint when it exceeds 2^53-1. */
    function Uvarint(buf: ByteView): [number | bigint, number];
    /** Decodes a signed varint as [value, bytesRead]; uvarint is the unsigned form. */
    function Varint(buf: ByteView): [number | bigint, number];
    /** Writes the varint at `offset` and returns the NEW END offset; a buffer too small throws RangeError and writes nothing (at most 10 bytes per value). */
    function appendUvarint(buf: Uint8Array, value: number | bigint, offset?: number): number;
    /** The varint value at `offset` without the tuple: Number up to 2^53-1, BigInt above; truncated input returns 0, 64-bit overflow throws RangeError. */
    function uvarintAt(buf: ByteView, offset?: number): number | bigint;
    /** varintAt: the zigzag signed form of uvarintAt. */
    function varintAt(buf: ByteView, offset?: number): number | bigint;

    /** Charset detection options. */
    interface DetectOptions {
        fallback?: string;
        allowList?: string[];
    }
    /** Deterministic charset detection over a byte view; throws TypeError when allowList excludes the verdict. */
    function DetectEncoding(data: ByteView, opts?: DetectOptions): string;
    /** Same as DetectEncoding under its lowercase name. */
    function detectEncoding(data: ByteView, opts?: DetectOptions): string;

    /** JSON5 superset parsing; depth capped at 256 (RangeError). JSON Pointer/Patch live in dyna:json. */
    function JSON5Parse(text: string): unknown;
    /** JSON5 output with unquoted keys and NaN/Infinity literals; indent clamped to 0-10. */
    function JSON5Stringify(value: unknown, opts?: { indent?: number }): string;
    /** RFC 8785 canonical JSON; NaN/Infinity rejected. The canonical form is always compact, so `indent` is accepted and ignored. */
    function StableStringify(value: unknown, opts?: { indent?: number }): string;

    /** A compiled RFC 9535 JSONPath: the PATTERN query language (wildcards, filters, `$..x`); use dyna:json's Pointer for one exact address. */
    class JSONPath {
        /** Compiles the expression once and reuses it; a syntax error or an expression longer than 4096 bytes throws. */
        constructor(expression: string);
        /** Every match. */
        all(value: unknown): unknown[];
        /** The first match, or undefined when none. */
        first(value: unknown): unknown;
        /** Normalized path strings like $['store']['book'][0]['author']. */
        paths(value: unknown): string[];
    }

    /** QR options: ecc L/M/Q/H, version 1-40, mask 0-7. */
    interface QROptions {
        ecc?: "L" | "M" | "Q" | "H";
        version?: number;
        mask?: number;
    }
    /** Renders a QR symbol; a symbol holds at most 2953 bytes. */
    function QREncode(text: string, opts?: QROptions): { version: number; size: number; modules: Uint8Array };
    /** The same symbol as text with two half-blocks per cell. */
    function QRToString(text: string, opts?: QROptions): string;
}

/** dyna:hash — digests and checksums, one-shot or streaming (Hasher): MD5, SHA-1/2/3, Keccak, SHAKE, BLAKE2/3, CRC32 (hardware-accelerated), xxHash. */
declare module "dyna:hash" {
    /** Keyed options: BLAKE2b keys are 0..64 bytes, BLAKE2s 0..32; an EMPTY key is legal and means unkeyed (RFC 7693). */
    interface Blk2Opts {
        key?: BytesInput;
        length?: number;
    }
    /** BLAKE3 keys are exactly 32 bytes; {context} (alias {deriveKey}) runs derive-key mode, whose output is exactly 32 bytes. */
    interface Blk3Opts {
        key?: BytesInput;
        length?: number;
        context?: string;
        deriveKey?: string;
    }
    /** MD5 (RFC 1321). */
    function MD5(data: BytesInput): Uint8Array;
    function MD5Hex(data: BytesInput): string;
    /** SHA-1 (FIPS 180-4). */
    function SHA1(data: BytesInput): Uint8Array;
    function SHA1Hex(data: BytesInput): string;
    /** SHA-224. */
    function SHA224(data: BytesInput): Uint8Array;
    function SHA224Hex(data: BytesInput): string;
    /** SHA-256, the engine's default digest. */
    function SHA256(data: BytesInput): Uint8Array;
    function SHA256Hex(data: BytesInput): string;
    /** SHA-384. */
    function SHA384(data: BytesInput): Uint8Array;
    function SHA384Hex(data: BytesInput): string;
    /** SHA-512. */
    function SHA512(data: BytesInput): Uint8Array;
    function SHA512Hex(data: BytesInput): string;
    /** IEEE 802.3 CRC-32 as a non-negative number. */
    function CRC32(data: BytesInput): number;
    /** CRC-32C (Castagnoli polynomial). */
    function CRC32C(data: BytesInput): number;
    /** Adler-32 (RFC 1950), the zlib stream checksum. */
    function Adler32(data: BytesInput): number;
    /** Content-defined chunk END offsets (FastCDC gear hash with normalised masks); defaults min 2048, avg 8192 (a power of two), max 65536.
     *  Inserting bytes moves only the chunk boundaries near the edit. */
    function CDCChunks(data: BytesInput, opts?: { min?: number; avg?: number; max?: number }): Uint32Array;
    /** FIPS 202 SHA-3, 224..512 bits. */
    function SHA3_224(data: BytesInput): Uint8Array;
    function SHA3_224Hex(data: BytesInput): string;
    function SHA3_256(data: BytesInput): Uint8Array;
    function SHA3_256Hex(data: BytesInput): string;
    function SHA3_384(data: BytesInput): Uint8Array;
    function SHA3_384Hex(data: BytesInput): string;
    function SHA3_512(data: BytesInput): Uint8Array;
    function SHA3_512Hex(data: BytesInput): string;
    /** Original Keccak padding, the form Ethereum uses. */
    function Keccak256(data: BytesInput): Uint8Array;
    function Keccak256Hex(data: BytesInput): string;
    /** SHAKE128 extensible output; length 1..2^20 bytes. */
    function SHAKE128(data: BytesInput, length?: number): Uint8Array;
    function SHAKE128Hex(data: BytesInput, length?: number): string;
    function SHAKE256(data: BytesInput, length?: number): Uint8Array;
    function SHAKE256Hex(data: BytesInput, length?: number): string;
    /** BLAKE3 Merkle-tree hash; length 1..2^20 bytes; an options object in the second position carries {key} / {context}. */
    function BLAKE3(data: BytesInput, length?: number, opts?: never): Uint8Array;
    function BLAKE3(data: BytesInput, opts?: Blk3Opts): Uint8Array;
    function BLAKE3Hex(data: BytesInput, length?: number, opts?: never): string;
    function BLAKE3Hex(data: BytesInput, opts?: Blk3Opts): string;
    /** BLAKE2b; 1..64 bytes.: {key}. */
    function BLAKE2b(data: BytesInput, length?: number, opts?: never): Uint8Array;
    function BLAKE2b(data: BytesInput, opts?: Blk2Opts): Uint8Array;
    function BLAKE2bHex(data: BytesInput, length?: number, opts?: never): string;
    function BLAKE2bHex(data: BytesInput, opts?: Blk2Opts): string;
    /** BLAKE2s; 1..32 bytes.: {key}. */
    function BLAKE2s(data: BytesInput, length?: number, opts?: never): Uint8Array;
    function BLAKE2s(data: BytesInput, opts?: Blk2Opts): Uint8Array;
    function BLAKE2sHex(data: BytesInput, length?: number, opts?: never): string;
    function BLAKE2sHex(data: BytesInput, opts?: Blk2Opts): string;
    /** Murmur3_128; the second argument is a seed, not a length. */
    function Murmur3_128(data: BytesInput, seed?: number): Uint8Array;
    function Murmur3_128Hex(data: BytesInput, seed?: number): string;
    /** 32-bit xxHash as a number. */
    function XXHash32(data: BytesInput, seed?: number): number;
    /** 64-bit xxHash as 16 hex characters by default; {as: "bigint" | "bytes"} returns the exact value or its little-endian bytes. */
    function XXHash64(data: BytesInput, seed?: number, opts?: { as?: "hex" | "bigint" | "bytes" }): string | bigint | Uint8Array;
    /** XXH3-64 (default secret), the modern cousin of XXHash64; NOT a security primitive. */
    function XXH3_64(data: BytesInput, seed?: number, opts?: { as?: "hex" | "bigint" | "bytes" }): string | bigint | Uint8Array;

    /** Streaming digest over every one-shot algorithm name ("sha256", "blake3", ...); {length} sizes the extensible-output ones (shake*, blake2*, blake3). */
    class Hasher implements DynResource {
        constructor(algorithm: string, opts?: { length?: number });
        /** Absorbs bytes; returns this for chaining. */
        update(data: BytesInput): this;
        /** Finalizes a copy; the stream stays usable. */
        digest(): Uint8Array;
        /** The same as a hex string. */
        digestHex(): string;
        /** Finalizes a copy into the caller's buffer and returns the byte count; RangeError when it is smaller than digestSize; the hasher stays usable. */
        digestInto(buf: Uint8Array): number;
        /** Returns the hasher to its initial state. */
        reset(): void;
        readonly algorithm: string;
        readonly digestSize: number;
        /** Releases the native state early (idempotent); every method and getter throws TypeError afterwards. */
        close(): void;
        dispose(): void;
        readonly closed: boolean;
        readonly [Symbol.dispose]: () => void;
    }
}
/** dyna:file — the filesystem: Path values, whole-file and buffered IO, atomic writes, locks, watchers, globs, temp files, platform directories. */
declare module "dyna:file" {
    /** A value handle over one normalized path; immutable, refcounted. Every dyna:file entry point that takes a path accepts a Path or a string; a string is converted exactly as new Path(s) would (same normalization, same errors), and any other value is a TypeError naming the Path argument. */
    class Path {
        /** Joins and normalizes the segments; at least one segment is required. An absolute result then has its parent directory canonicalized through realpath(3): symlinks in the parent are followed (on macOS /tmp/x becomes /private/tmp/x); a parent that does not exist leaves the path as joined; the final component is never followed, except when the parent is "/" and the whole path resolves. dirname and every Path the module returns (cwd, home, temp, makeTempDir, glob, ...) get the same treatment; join() and resolve() are lexical, so new Path(p, "x") and p.join("x") differ when p ends in a symlink; relative paths are untouched. Kept deliberately: one realpath per absolute Path. */
        constructor(first: string | Path, ...segments: (string | Path)[]);
        /** Current working directory. */
        static cwd(): Path;
        /** $HOME, else the passwd entry. */
        static home(): Path;
        /** $TMPDIR, else /tmp, trailing slash stripped. */
        static temp(): Path;
        /** True when `v` is a Path. */
        static isPath(v: unknown): v is Path;
        /** "/" on this platform. */
        static readonly sep: string;
        /** ":" on this platform. */
        static readonly delimiter: string;
        /** The parent directory as a new Path. */
        get dirname(): Path;
        /** The final component as a string. */
        get basename(): string;
        /** The extension, including the dot. */
        get extname(): string;
        /** Whether the path is absolute. */
        get isAbsolute(): boolean;
        /** `this` leads, then the arguments; returns a new Path. */
        join(...segments: (string | Path)[]): Path;
        /** join that resolves `.`/`..` LEXICALLY (absolute argument rebases); no filesystem/symlink access; NUL in a segment is a TypeError. The result drops a trailing slash. A relative result stays relative and is never rooted at "/" or the cwd: new Path("tools/x.js").resolve() is "tools/x.js", new Path("a").resolve("../../b") is "../b", an empty result is ".". (Before B11 a relative receiver came back rooted at "/".) */
        resolve(...segments: (string | Path)[]): Path;
        /** The relative path from `this` to `other`. */
        relativeTo(other: Path | string): Path;
        /** Byte equality of the normalized forms; false for a non-Path argument. */
        equals(other: unknown): boolean;
        /** Basename with the given suffix removed. */
        basenameWithout(suffix: string): string;
        toString(): string;
        toJSON(): string;
    }

    /** stat/lstat result. */
    interface Stat {
        size: number;
        mode: number;
        isDir: boolean;
        isFile: boolean;
        isSymlink: boolean;
        mtimeMs: number;
        atimeMs: number;
        ctimeMs: number;
        uid: number;
        gid: number;
        ino: number;
        nlink: number;
    }

    /** A handle over one path; every method is the free function with the path supplied once. */
    class File {
        /** Accepts a Path or a string. */
        constructor(path: Path | string);
        readonly path: Path;
        /** Whole file as a string; invalid UTF-8 becomes U+FFFD. */
        readText(): string;
        /** Whole file as a Uint8Array. */
        readBytes(): Uint8Array;
        /** Write string or bytes; {append:true} appends; returns the byte count. */
        writeText(data: BytesInput, opts?: { append?: boolean }): number;
        /** Alias of writeText. */
        writeBytes(data: BytesInput, opts?: { append?: boolean }): number;
        /** writeText with append supplied. */
        append(data: BytesInput): number;
        /** File metadata, following symlinks (lstat() describes the link itself). */
        stat(): Stat;
        lstat(): Stat;
        /** Boolean; never throws. */
        exists(): boolean;
        /** Unlink the file or empty directory. */
        remove(): void;
        /** The resolved Path. */
        realPath(): Path;
        /** Change permissions. */
        chmod(mode: number): void;
        /** Rename; on success the handle names the new location. */
        moveTo(dest: Path | string): this;
        /** Byte copy to a new File; refuses an existing destination unless `{overwrite: true}`. */
        copyTo(dest: Path | string, opts?: { overwrite?: boolean }): File;
        /** Opens a buffered FileReader on this file. */
        reader(opts?: { bufferSize?: number }): FileReader;
        /** Opens a buffered FileWriter; `append` adds to the end, `preallocate` reserves bytes up front. */
        writer(opts?: { bufferSize?: number; preallocate?: number; append?: boolean }): FileWriter;
        toString(): string;
        toJSON(): string;
    }

    /** Buffered sequential reader. read/readLine/readAll are STRING paths (UTF-8, invalid bytes become U+FFFD); readInto/readBytes are raw BYTE paths. */
    class FileReader implements DynResource {
        /** bufferSize defaults to 128 KiB (clamped to 4 KiB..64 MiB). */
        constructor(path: Path | string, opts?: { bufferSize?: number });
        /** Up to `n` bytes as a string, "" at EOF; `n` omitted reads all. */
        read(n?: number): string;
        /** The next line without its trailing newline; null at a clean EOF. */
        readLine(): string | null;
        /** The rest of the file. */
        readAll(): string;
        /** Copies up to `buf.length` raw bytes into `buf`; returns the count, 0 = EOF. No UTF-8 repair. */
        readInto(buf: ByteView): number;
        /** Up to `n` raw bytes as a fresh Uint8Array (empty at EOF); `n` omitted reads all. */
        readBytes(n?: number): Uint8Array;
        close(): void;
        dispose(): void;
        readonly closed: boolean;
        readonly [Symbol.dispose]: () => void;
    }

    /** Buffered sequential writer; flushes buffered bytes on teardown. */
    class FileWriter implements DynResource {
        /** bufferSize defaults to 128 KiB; `append` adds to the end; `preallocate` reserves bytes up front. */
        constructor(path: Path | string, opts?: { bufferSize?: number; preallocate?: number; append?: boolean });
        /** Accepts a string, ArrayBuffer, or any TypedArray/DataView; returns bytes accepted. */
        write(data: BytesInput): number;
        /** Push buffered bytes to the fd. */
        flush(): void;
        /** Flush then durable-sync (F_FULLFSYNC on Darwin). */
        sync(): void;
        close(): void;
        dispose(): void;
        readonly closed: boolean;
        readonly [Symbol.dispose]: () => void;
    }

    /** An advisory exclusive lock via flock(2). */
    class FileLock implements DynResource {
        /** `retry` extra attempts (default 0: fail at once when held), spaced `retryMs` apart (default 100). */
        constructor(path: Path | string, opts?: { retry?: number; retryMs?: number });
        /** Calls fn, then releases the lock no matter what fn did; the lock is consumed. */
        withLock<T>(fn: () => T): T;
        close(): void;
        dispose(): void;
        readonly closed: boolean;
        readonly [Symbol.dispose]: () => void;
    }

    /** Kernel-event file watching (kqueue/inotify); events { path, kind } reach both the start() callback and async iteration. Each `ignore` pattern is matched like Glob (braces expand; past 4096 alternatives the constructor throws RangeError) against the relative path and the bare name. */
    class Watcher implements DynResource {
        constructor(path: Path | string, opts?: { recursive?: boolean; debounceMs?: number; ignore?: string[] });  /* opts must be an object when given: a string/number throws TypeError */
        /** Arms the watch (path is a plain string); a restart reopens the event stream a stop() ended. */
        start(cb?: (event: { path: string; kind: "change" | "add" | "addDir" | "unlink" | "unlinkDir" }) => void): void;
        /** Halts the watch and ends the stream: buffered events still drain through next(), then { done: true }. Idempotent; start() may follow. */
        stop(): void;
        /** One async pull: a Promise of { value, done }; a parked pull resolves done on stop()/close(). */
        next(): Promise<{ value: { path: string; kind: string }; done: false } | { value: undefined; done: true }>;
        /** What a for-await `break` calls: ends the iteration now (buffered events dropped) and stops the watch. */
        return(): Promise<{ value: undefined; done: true }>;
        /** The watcher itself, so `for await (const ev of w)` works. */
        [Symbol.asyncIterator](): AsyncIterator<{ path: string; kind: string }>;
        /** `{ entries, directories, events, truncated, debounceMs }`. */
        stats(): { entries: number; directories: number; events: number; truncated: boolean; debounceMs: number };
        close(): void;
        dispose(): void;
        readonly closed: boolean;
        readonly [Symbol.dispose]: () => void;
    }

    /** A compiled glob for LEXICAL whole-path matching (no filesystem access): braces expand as in glob() (a path matches if any alternative does; past 4096 alternatives the constructor throws RangeError), `*`, `**` and `?` all cross `/` and no leading-dot rule applies, unlike glob()'s walk; a `**` followed by `/` needs that slash, so the recursive `.tmp` pattern misses a top-level `a.tmp` that `*.tmp` matches. */
    class Glob {
        constructor(pattern: string);
        /** Lexical match only, no filesystem access. */
        matches(path: Path | string): boolean;
        /** One-shot `new Glob(pattern).matches(path)`; a Path contributes its normalised bytes, a string is used verbatim. */
        static match(path: Path | string, pattern: string): boolean;
        /** The glob() walk with an optional cwd; returns matching Paths. */
        expand(cwd?: Path | string): Path[];
        /** The subset of the array matching the pattern. */
        filter<T extends Path | string>(paths: T[]): T[];
        readonly pattern: string;
        /** True when the pattern has `*`, `?`, `[` or a brace group that expands to more than one alternative. */
        readonly hasWildcard: boolean;
    }

    /** Whole file as a string (invalid UTF-8 becomes U+FFFD); `{bytes: true}` returns a Uint8Array. Symlinks anywhere in the path are followed. */
    function readFile(path: Path | string, opts?: { bytes?: boolean }): string | Uint8Array;
    /** Whole file as a Uint8Array; same as `readFile(path, {bytes: true})`. */
    function readBytes(path: Path | string): Uint8Array;
    /** Write string or bytes; O_CREAT, truncate unless append; returns the byte count; undefined/null is a TypeError. A string is written as well-formed UTF-8: a lone surrogate becomes U+FFFD (EF BF BD), as TextEncoder does.
     *  `{atomic: true}` never exposes a half-written file: the data goes to a new temporary file beside the target, is fsync'd, then renamed over it (and the directory fsync'd), so a reader or a crash sees the old content or the new, never a mix.
     *  The result is a NEW inode (an open handle or hard link keeps the old content); an existing target's permission bits carry over, its owner does not; a symlinked path replaces the link's target, not the link. On failure the target is untouched and the temporary removed. With append: TypeError. */
    function writeFile(path: Path | string, data: BytesInput, opts?: { append?: boolean; atomic?: boolean }): number;
    /** Promise twins of readFile, readBytes, writeFile and the 14 path calls (stat..chmod, same results and error shapes), plus open(), reader() and writer(); transitional, F5 gives them the default names. Disk I/O and UTF-8 repair run on the I/O pool.
     *  Argument errors throw synchronously; I/O errors reject with the sync error shape; a pending request rejects at engine shutdown. 4 KiB read: 22 us awaited one by one (sync 14 us, empty pool hop 3.6 us), 13 us with 64 in flight; RSS +0.4 MiB, flat. */
    namespace async {
        /** A relative path resolves against the cwd when the pool runs the request, not at the call. With no I/O reactor the call rejects InternalError; no disk I/O runs on the JS thread. */
        function readFile(path: Path | string, opts?: { bytes?: boolean }): Promise<string | Uint8Array>;
        function readBytes(path: Path | string): Promise<Uint8Array>;
        /** Un-awaited requests run in parallel in no set order and the last to finish wins; an atomic write is always one whole payload, plain writes to one path may interleave. */
        function writeFile(path: Path | string, data: BytesInput, opts?: { append?: boolean; atomic?: boolean }): Promise<number>;
        /** Sync stat/lstat/exists stay (2x rule): warm-cache p50 sync 2 us vs async 6-16 us awaited (>=3x), 2-7 us each with 64 in flight; N=16..1M, peak RSS 7.3 MiB flat.
         *  A sync call blocks the JS thread on a slow or network filesystem; use async there. */
        function stat(path: Path | string): Promise<Stat>;
        function lstat(path: Path | string): Promise<Stat>;
        function exists(path: Path | string): Promise<boolean>;
        function readDir(path: Path | string): Promise<{ name: string; isDir: boolean; isFile: boolean; isSymlink: boolean }[]>;
        function makeDir(path: Path | string, opts?: { recursive?: boolean; mode?: number }): Promise<void>;
        function remove(path: Path | string): Promise<void>;
        function removeAll(path: Path | string): Promise<void>;
        function rename(from: Path | string, to: Path | string): Promise<void>;
        function copyFile(from: Path | string, to: Path | string, opts?: { overwrite?: boolean }): Promise<number>;
        function move(from: Path | string, to: Path | string): Promise<void>;
        function symlink(target: string, linkpath: Path | string): Promise<void>;
        function readLink(path: Path | string): Promise<string>;
        function realPath(path: Path | string): Promise<Path>;
        function chmod(path: Path | string, mode: number): Promise<void>;
        /** flags "r" (default) | "r+" | "w" | "w+" | "wx" | "a" | "a+", O_CLOEXEC always; mode 0..0o7777 (default 0o666, else RangeError) for a created file. */
        function open(path: Path | string, opts?: { flags?: "r" | "r+" | "w" | "w+" | "wx" | "a" | "a+"; mode?: number }): Promise<FileHandle>;
        /** An open fd from open(): its calls run one at a time on the I/O pool in call order; after close() every call rejects EBADF.
         *  Dropped unclosed, GC closes the fd on the JS thread; at engine shutdown queued calls reject and the fd closes after the in-flight one. */
        interface FileHandle {
            /** Up to n bytes (default 65536, 0..2^30 else RangeError), short only at EOF; pos reads there without moving the cursor. */
            read(n?: number, pos?: number): Promise<Uint8Array>;
            /** Fills view (copied in at completion: only what still fits a shrunk view; a detached view rejects TypeError); resolves the count. */
            read(view: ByteView, pos?: number): Promise<number>;
            /** Writes all of data (copied at the call); pos writes there without moving the cursor (O_APPEND handles write at the end). */
            write(data: BytesInput, pos?: number): Promise<number>;
            /** Sync lseek, whence "set" (default) | "cur" | "end"; returns the new offset; TypeError while a call is queued or in flight. */
            seek(offset: number, whence?: "set" | "cur" | "end"): number;
            /** F_FULLFSYNC on Darwin (fsync where refused), fsync elsewhere. */
            sync(): Promise<void>;
            stat(): Promise<Stat>;
            /** Runs after the queued calls; a second close() resolves at once. */
            close(): Promise<void>;
            readonly closed: boolean;
            [Symbol.asyncDispose](): Promise<void>;
        }
        /** Buffered FileReader twin; bufferSize as FileReader (128 KiB, clamped 4 KiB..64 MiB); open, fills and close run on the I/O pool.
         *  With nothing queued, a line or read(n) already buffered settles on the JS thread (tens of ns vs a ~4 us pool hop, measured). */
        function reader(path: Path | string, opts?: { bufferSize?: number }): Promise<AsyncFileReader>;
        /** Buffered FileWriter twin (mode 0644, truncates unless append); preallocate reserves bytes, failure ignored.
         *  With nothing queued, a write that fits the buffer is copied on the JS thread; flushes and larger writes run on the I/O pool. */
        function writer(path: Path | string, opts?: { bufferSize?: number; preallocate?: number; append?: boolean }): Promise<AsyncFileWriter>;
        /** Calls queue in order; strings repair invalid UTF-8 to U+FFFD; after close, reads reject EBADF and next/return resolve done. */
        interface AsyncFileReader {
            /** Up to n bytes (0..2^30 else RangeError) as a string, "" at EOF; n omitted reads all, over 2^30 rejects RangeError. */
            read(n?: number): Promise<string>;
            /** The rest of the file; over 2^30 bytes rejects RangeError. */
            readAll(): Promise<string>;
            /** The next line without \n or \r\n; null at a clean EOF; a line over 2^30 bytes rejects RangeError. */
            readLine(): Promise<string | null>;
            /** Fills view with raw bytes (copied in at completion, only what still fits; detached rejects TypeError); resolves the count, 0 = EOF. */
            readInto(view: ByteView): Promise<number>;
            /** Up to n raw bytes (0..2^30 else RangeError); n omitted reads all. */
            readBytes(n?: number): Promise<Uint8Array>;
            /** The next line as an iterator result; done at EOF. */
            next(): Promise<IteratorResult<string, undefined>>;
            /** Closes the reader (for-await break calls it); resolves done. */
            return(): Promise<IteratorResult<string, undefined>>;
            lines(): AsyncFileReader;
            [Symbol.asyncIterator](): AsyncFileReader;
            /** Runs after the queued calls; a second close() resolves at once. */
            close(): Promise<void>;
            readonly closed: boolean;
            [Symbol.asyncDispose](): Promise<void>;
        }
        /** Calls queue in order; buffered bytes are flushed at close, at runtime teardown and when collected; after close, calls reject EBADF. */
        interface AsyncFileWriter {
            /** Copies data at the call; resolves bytes accepted; data of at least bufferSize goes straight to the fd. */
            write(data: BytesInput): Promise<number>;
            /** Pushes buffered bytes to the fd. */
            flush(): Promise<void>;
            /** Flush then F_FULLFSYNC on Darwin (fsync where refused), fsync elsewhere. */
            sync(): Promise<void>;
            /** Flushes then closes after the queued calls; a second close() resolves at once. */
            close(): Promise<void>;
            readonly closed: boolean;
            [Symbol.asyncDispose](): Promise<void>;
        }
    }

    /** Follows the final component. */
    function stat(path: Path | string): Stat;
    /** Does not follow the final component. */
    function lstat(path: Path | string): Stat;
    /** Boolean; never throws; uses lstat, so a dangling symlink reports true. */
    function exists(path: Path | string): boolean;

    /** Sorted entries; `.`/`..` excluded. */
    function readDir(path: Path | string): { name: string; isDir: boolean; isFile: boolean; isSymlink: boolean }[];
    /** Create a directory; recursive creates missing parents. */
    function makeDir(path: Path | string, opts?: { recursive?: boolean; mode?: number }): void;
    /** Unlink a file or an empty directory. */
    function remove(path: Path | string): void;
    /** Recursive, symlink-safe removal; a missing path is a no-op. */
    function removeAll(path: Path | string): void;
    /** rename(2). */
    function rename(from: Path | string, to: Path | string): void;
    /** Byte copy through the kernel; refusing an existing destination is the default. Returns the source byte count. */
    function copyFile(from: Path | string, to: Path | string, opts?: { overwrite?: boolean }): number;
    /** rename(2), falling back to copy-then-unlink across filesystems; refuses a destination that resolves to the same file. */
    function move(from: Path | string, to: Path | string): void;
    /** MIME type from magic bytes, not the extension. */
    function sniffType(pathOrBytes: Path | string | ByteView): string;

    /** Creates the link; the link location is a Path or a string (converted like new Path), the target is stored verbatim; a target with an embedded NUL is a TypeError. */
    function symlink(target: string, linkpath: Path | string): void;
    /** The stored target verbatim, as a string. */
    function readLink(path: Path | string): string;
    /** The fully resolved path as a Path. */
    function realPath(path: Path | string): Path;
    /** Changes permissions. */
    function chmod(path: Path | string, mode: number): void;

    /** Walks the filesystem and returns sorted Paths: `*` stays within one segment, `**` spans any number (never through symlinked directories), `?`, `[a-z]`, `[!x]`.
     *  Braces expand first, as in bash: `{a,b}`, nested `{a,{b,c}}`, several groups and empty alternatives (`x{,.bak}`) each become a pattern and the results are merged, sorted and de-duplicated; a group with no top-level comma (`{x}`), an unbalanced `{` and `{1..3}` stay literal, a `{` inside `[..]` is literal (write `[{]` for a literal brace), and more than 4096 expansions throw RangeError.
 *  Wildcards skip dotfiles unless the segment starts with `.`; `{cwd}` roots the walk and the returned Paths are RELATIVE to it (re-join with cwd before stat/readFile). */
    function glob(pattern: string, opts?: { cwd?: Path | string }): Path[];

    /** The system temp directory as a Path. */
    function tempDir(): Path;
    /** mkdtemp under the temp dir; the prefix is a plain name fragment (a slash, a leading `.` or more than 200 bytes is a RangeError). */
    function makeTempDir(prefix?: string): Path;
    /** mkstemp; returns the path of the empty file. The file is created with O_CREAT|O_EXCL and mode 0600: the name did not exist before the call, so no existing file is ever opened, truncated or returned, and only the owner can read or write it. */
    function makeTempFile(prefix?: string): Path;
    /** Per-user platform data directory. */
    function dataDir(app?: string): Path;
    /** Per-user platform config directory. */
    function configDir(app?: string): Path;
    /** Per-user platform cache directory. */
    function cacheDir(app?: string): Path;
    /** System-wide data directory variant. */
    function dataDirSite(app?: string): Path;
    function configDirSite(app?: string): Path;
    function cacheDirSite(app?: string): Path;
}

/** dyna:html — server-side web content: an HTML5 parser into a shared element tree, CSS-selector queries, an allow-list sanitizer, markdown, templates. */
declare module "dyna:html" {
    /** An element node, the SHARED tree shape: HTMLParse produces it, dyna:scrape's Extractor and Crawl consume it, dyna:xml uses the same layout. */
    interface HTMLElement {
        name: string;
        attrs: Record<string, string>;
        children: (string | HTMLElement)[];
    }

    /** Parses HTML into an array of root nodes. */
    function HTMLParse(text: string): HTMLElement[];
    /** Serializes a node back to HTML. */
    function HTMLStringify(node: HTMLElement): string;
    /** Extracts the visible text of a node or node array. */
    function HTMLText(node: HTMLElement | HTMLElement[]): string;
    /** Renders Markdown to HTML; allowRawHTML defaults FALSE (raw HTML is escaped; true is the trusted-input opt-in).
     *  headingIds (default false) gives every heading an id slugged from its rendered text: tags and character
     *  references dropped, ASCII lowercased, each space -> "-", letters/digits/"-"/"_"/non-ASCII kept, other ASCII
     *  dropped; a repeat gets "-1", "-2"... (github-slugger's rule); an empty slug gets no id. */
    function MarkdownToHTML(text: string, opts?: { allowRawHTML?: boolean; headingIds?: boolean }): string;

    /** A compiled CSS selector over the parsed tree. */
    class Selector {
        constructor(text: string);
        /** Every matching element. */
        all(doc: HTMLElement | HTMLElement[]): HTMLElement[];
        /** The first match, or undefined when none. */
        first(doc: HTMLElement | HTMLElement[]): HTMLElement | undefined;
        /** True when the single node matches (no combinators allowed). */
        matches(node: HTMLElement): boolean;
    }

    /** Allow-list sanitizer; `protocols` maps "tag.attr" to permitted URL schemes. Output is re-escaped, so re-parsing the output introduces no new structure. */
    class Sanitizer {
        /** An allow-list is required; there is no default policy. */
        constructor(opts: { allow: Record<string, unknown>; protocols?: Record<string, string[]> });
        /** Returns the sanitized HTML. */
        clean(html: string): string;
    }

    /** Rewrites URL-bearing attributes (href, src, srcset candidates, ...) IN PLACE through fn(url, tag, attr); null/undefined keeps the original; returns the count replaced. */
    function rewriteLinks(doc: HTMLElement | HTMLElement[], fn: (url: string, tag: string, attr: string) => unknown): number;

    /** A compiled template; `escape` defaults to TRUE ({escape:false} is the trusted-input opt-in). Caps: 65536 tags, sections 64 deep. */
    class Template {
        constructor(source: string, opts?: { escape?: boolean });
        /** Renders with the data scope; at most 1048576 section elements per render (RangeError, not a hang). */
        render(data?: unknown): string;
    }
}

/** dyna:http — the web platform: WHATWG fetch/Request/Response/Headers/FormData, HTTP servers (App, HTTPServer, HTTPServerAsync), WebSockets, SSE, header codecs.
 *  Its own module since B1e: dyna:net no longer re-exports it. The global fetch gets its transport from this module; a build without it rejects http(s) fetch with a TypeError naming "http". */
declare module "dyna:http" {
    /** A parsed Content-Type header. */
    interface ContentType {
        type: string;
        subtype: string;
        parameters: Record<string, string>;
    }
    /** Parses a Content-Type header into {type, subtype, parameters}; null when malformed. */
    function ContentTypeParse(header: string): ContentType | null;
    /** Formats a Content-Type back to a header string. */
    function ContentTypeFormat(ct: ContentType): string;
    /** Content negotiation: the best candidate STRING for an Accept header, or null; q=0 excludes, ties follow the header's order; every candidate must be a string (a hole included, TypeError naming the index). Weights (RFC 9110 12.4.2) are read exactly as App's compress reads Accept-Encoding: BWS around "=" is tolerated ("q = 0" excludes), a q that is not a qvalue ("abc", "2", "1.5") is ignored (weight 1), and digits past the third never turn a non-zero q into 0. */
    function Negotiate(header: string, candidates: string[]): string | null;
    /** Picks the best supported token by q-value (weights read as Negotiate reads them); "*" selects the first candidate; candidates follow Negotiate's string rule. */
    function NegotiateToken(header: string, candidates: string[]): string | null;
    /** Parses a Range header against a size: inclusive {start, end} ranges, "unsatisfiable" when none fits, null when malformed. */
    function RangeParse(header: string, size: number): { start: number; end: number }[] | "unsatisfiable" | null;
    /** Parses a Cookie header into an object; a repeated name keeps the LAST value. */
    function CookieParse(header: string): Record<string, string>;
    /** CookieSerialize options; STRICT bag (an unknown key is a TypeError naming it). */
    interface CookieOptions {
        maxAge?: number;
        domain?: string;
        path?: string;
        sameSite?: "Strict" | "Lax" | "None" | "strict" | "lax" | "none";
        secure?: boolean;
        httpOnly?: boolean;
    }
    /** Serializes a cookie; absent options emit exactly `name=value`. */
    function CookieSerialize(name: string, value: string, opts?: CookieOptions): string;
    /** True when the If-None-Match header matches the etag. */
    function ETagMatch(header: string, etag: string): boolean;
    /** Parses a multipart body against the boundary in the Content-Type header. */
    function MultipartParse(contentType: string, body: ByteView): { name: string; filename?: string; body: Uint8Array }[];
    /** Formats multipart parts; returns {body, contentType}. */
    function MultipartFormat(parts: { name: string; value?: string; body?: ByteView; filename?: string }[], boundary?: string): { body: Uint8Array; contentType: string };

    /** A non-blocking HTTP/1.1 client on the event loop's reactor (T1, 2026-11-26: the blocking get/post/request/getStream were removed, a clean break). Every exchange returns a Promise and never blocks the JS thread, so a client can call an App, HTTPServer or TCPServer in the same process. It shares fetch()'s transport and connection pool: requests to one origin reuse idle pooled connections (kept at most 4 s, 256 per process); at most 64 exchanges per origin (host, port, scheme) are in flight at once per thread by default (dyna.maxPerOrigin raises or removes the cap) and further requests wait FIFO for a freed slot (RFC 9112 9.4; it also keeps one origin from overflowing a listen backlog, 128 by default on macOS), the wait is not cut by the per-read timeout but is bounded by the total deadline (20x the timeout); a getStream response gives back its slot once its head has arrived, so open streams never starve other requests; a reused connection that died before any response byte is retried once on a fresh one only for replayable requests (GET, HEAD, OPTIONS, or an Idempotency-Key header); a non-replayable request lost that way rejects (RFC 9110 9.2.2) and the origin is remembered, so the next one opens its own connection. Choose this over fetch for explicit timeout/maxBody/onConnect control and a plain {status, headers, body} result; scrape's Fetcher runs its policy over this client. Argument errors (url, method, header names/values, a closed client, https without the tls module) throw synchronously; network, framing and size errors reject. */
    /** dyna meta configuration of HTTPClient and fetch() (an extension beyond RFC 9110/9112), a strict bag: an unknown key is a TypeError. */
    interface HTTPClientMetaOptions {
        /** The most exchanges this client keeps in flight to one origin (scheme, host, port) at once; default 64. An integer in [1, 2^31-1] (RangeError otherwise, TypeError for a non-number) or Infinity for no cap. The origin table is shared per thread: an exchange is admitted while the origin's total in flight, across every client and fetch(), is under its OWN cap, otherwise it waits FIFO; an Infinity caller never waits, and it never admits a capped caller past that caller's cap. */
        maxPerOrigin?: number;
    }

    class HTTPClient implements DynResource {
        /** maxBody caps the buffered response (default 16 MiB); a response over it throws RangeError. HTTPS verifies certificate and hostname against the platform trust store. opts is a strict bag (unknown key: TypeError) holding only the dyna meta bag, see HTTPClientMetaOptions. */
        constructor(maxBody?: number, opts?: { dyna?: HTTPClientMetaOptions });
        /** GET resolving to {status, statusText, ok, headers, body}. Rejects with RangeError when the response passes maxBody. */
        get(url: string, headers?: Record<string, string>): Promise<HTTPResponse>;
        /** POST with an optional body; same contract as get(). */
        post(url: string, body?: BytesInput, headers?: Record<string, string>): Promise<HTTPResponse>;
        /** Any method; same contract as get(). */
        request(method: string, url: string, body?: BytesInput, headers?: Record<string, string>): Promise<HTTPResponse>;
        /** Called once per request with the RESOLVED IP between connect and the first request byte; return false or throw to refuse the connection. */
        onConnect?: (ip: string) => unknown;
        /** Per-read/per-write socket timeout in ms (0 disables); a buffered exchange is also bounded as a whole at 20x this value. */
        setTimeout(ms: number): void;
        /** Cancels this client's in-flight exchanges (each rejects, requests still waiting for a connection slot and a getStream body's pending read included) and closes the idle pooled connections it opened; a request made afterwards runs normally. */
        disconnect(): void;
        /** Resolves once the response head arrives, with the body left on the connection: the StreamedResponse's read(buf) resolves as bytes arrive (one pending read at a time; a second rejects TypeError) and never blocks the JS thread. Bytes the server sends ahead of read() are buffered, bounded by maxBody, which counts the whole body (a chunked trailer section included); a declared Content-Length over maxBody rejects the getStream promise, a running total over it rejects read() with RangeError "HTTPBodyStream: body exceeds maxBodyBytes". Body framing and truncation errors after the head reject read(), not getStream. The per-read timeout (setTimeout) runs only while a read() is pending, so a long-lived stream can idle between reads. close() cancels the exchange and settles a pending read with 0. A stream read to its end leaves its connection in the pool like get(). Framing per RFC 9112 6: Transfer-Encoding is chunked only when chunked is its final coding; Content-Length with chunked, or two different Content-Length values, rejects "malformed response: ..."; Content-Length with a non-chunked coding frames by Content-Length, as request() does. Chunk sizes are strict 1*HEXDIG. */
        getStream(url: string, headers?: Record<string, string>): Promise<StreamedResponse>;
        /** Attaches a CookieJar (null detaches): every later get/post/request/getStream and fetch-style async exchange sends the jar's Cookie for its url unless the call passes its own Cookie header, and stores each response's Set-Cookie lines. The jar is shared, not copied. */
        useCookies(jar: CookieJar | null): void;
        close(): void;
        dispose(): void;
        readonly closed: boolean;
        readonly [Symbol.dispose]: () => void;
    }

    /** The buffered response an HTTPClient promise resolves to. */
    interface HTTPResponse {
        status: number;
        statusText: string;
        ok: boolean;
        /** One key per field name, spelled as the server first spelled it (names match case-insensitively). RFC 9110 5.3: a repeated list field joins as "v1, v2"; Set-Cookie lines join with "\n" (a cookie's Expires holds a comma); a single-value field (Content-Length, Content-Type, Location, ETag, Last-Modified, Date, Expires, Age, Retry-After, Content-Location, Content-Range, ...) repeated with a different value fails the request ("malformed response: conflicting <name> values"), an identical repeat is one value. Values lose surrounding whitespace. getStream, Fetcher and fetch() read heads the same way. Over https an unframed body must end with close_notify, else "truncated body". */
        headers: Record<string, string>;
        body: string;
    }

    /** The getStream response: head fields plus a read(buf)/close() body: lines() and ndjson() consume it directly; toReadableStream(body) makes it a ReadableStream (pipeTo, pipeThrough(new DecompressionStream(...))). */
    interface StreamedResponse {
        status: number;
        statusText: string;
        ok: boolean;
        headers: Record<string, string>;
        /** The final url of the exchange (one hop: no redirect chasing). */
        url: string;
        contentType: string;
        /** Fills the caller's buffer and resolves with the count; 0 = end of body. Rejects on truncation, the max body, or malformed chunked framing. */
        read(buf: Uint8Array): Promise<number>;
        close(): void;
        dispose(): void;
        readonly closed: boolean;
        readonly [Symbol.dispose]: () => void;
    }

    /** A static route table: path -> body or {status, contentType, body}. App runs JS handlers; HTTPServer and HTTPServerAsync serve these tables off-thread. */
    type StaticRoutes = Record<string, string | { status?: number; contentType?: string; body?: string }>;

    /** A static-route HTTP/1.1 server on its own reactor thread (any client in the same script can call it, as it can an App); binds in the constructor. Every final response (status 200 and up; not 101) carries one `Date: <IMF-fixdate>` line straight after the status line (RFC 9110 section 6.6.1, https://www.rfc-editor.org/rfc/rfc9110#section-6.6.1), taken from a per-thread line rebuilt at most once per second from the wall-clock second the thread's reactor read when it last woke (no clock read per response). */
    class HTTPServer implements DynResource {
        /** Defaults: port 0 (ephemeral, read it back from `.port`), all interfaces, requestTimeoutMs 30000; a request is capped at 16 KiB (431/413). `workers` (default 1, clamped to 1..64) is the number of reactor threads start() runs; each serves the connections it accepted whole, and stop() joins them all. On Linux each reactor accepts on its own SO_REUSEPORT listener: it helps from about 16 concurrent connections (with 4, the kernel's hash can put two on one reactor). On macOS the reactors share one listening socket: only pipelined or keep-alive loads that saturate one reactor gain, while unpipelined and connection-per-request traffic stays kernel-bound and costs more CPU per request. The 8192-connection cap applies per reactor. */
        constructor(opts?: {
            port?: number;
            host?: string;
            workers?: number;
            backlog?: number;
            requestTimeoutMs?: number;
            routes?: StaticRoutes;
        });
        /** Binds the listener and starts serving. A started server stays alive until stop() or close(), as in node, even when its handle is dropped (a GC root from start() to stop()/close()); an unstarted, stopped or closed one is collectable. */
        start(): void;
        /** Stops accepting, closes live connections, and joins every reactor thread; the server is no longer a GC root. */
        stop(): void;
        /** The bound port (the real one when constructed with port 0). */
        get port(): number;
        close(): void;
        dispose(): void;
        readonly closed: boolean;
        readonly [Symbol.dispose]: () => void;
    }

    /** A static-route server multiplexing every connection on one background kqueue/epoll thread; a request is capped at 1 MiB. Every final response (status 200 and up; not 101) carries one `Date: <IMF-fixdate>` line straight after the status line (RFC 9110 section 6.6.1, https://www.rfc-editor.org/rfc/rfc9110#section-6.6.1), taken from a per-thread line rebuilt at most once per second from the wall-clock second the thread's reactor read when it last woke (no clock read per response). */
    class HTTPServerAsync implements DynResource {
        /** Defaults: port 0 (ephemeral), all interfaces, idleTimeoutMs 30000 (restarted by a request consumed or response bytes the peer accepts), maxConns 8192 (0 = unbounded). `workers` (default 1, clamped to 1..64) is the number of reactor threads start() runs; each serves the connections it accepted whole, and stop() joins them all. Routes are static and served in C, so workers parallelise the whole request path. On Linux each reactor accepts on its own SO_REUSEPORT listener: it helps from about 16 concurrent connections (with 4, the kernel's hash can put two on one reactor). On macOS the reactors share one listening socket: only pipelined or keep-alive loads that saturate one reactor gain, while unpipelined and connection-per-request traffic stays kernel-bound and costs more CPU per request. maxConns applies per reactor. */
        constructor(opts?: {
            port?: number;
            host?: string;
            backlog?: number;
            idleTimeoutMs?: number;
            maxConns?: number;
            routes?: StaticRoutes;
            workers?: number;
        });
        /** Binds the listener and starts serving. A started server stays alive until stop() or close(), as in node, even when its handle is dropped (a GC root from start() to stop()/close()); an unstarted, stopped or closed one is collectable. */
        start(): void;
        /** Stops accepting, closes live connections, and joins every reactor thread; the server is no longer a GC root. */
        stop(): void;
        /** The bound port (the real one when constructed with port 0). */
        get port(): number;
        close(): void;
        dispose(): void;
        readonly closed: boolean;
        readonly [Symbol.dispose]: () => void;
    }

    /** A routed application server: handlers are plain JS on the event-loop thread through the shared reactor; binds at start(). Every final response (status 200 and up; not 101) carries one `Date: <IMF-fixdate>` line straight after the status line (RFC 9110 section 6.6.1, https://www.rfc-editor.org/rfc/rfc9110#section-6.6.1), taken from a per-thread line rebuilt at most once per second from the wall-clock second the thread's reactor read when it last woke (no clock read per response). A handler that sets its own Date header (any case) has it sent instead, once. */
    class App implements DynResource {
        /** Defaults: port 0, all interfaces, idleTimeoutMs 30000, maxConns 8192, compress true, metrics false. The idle clock restarts on a request consumed and on response bytes the peer accepts, so a download slower than idleTimeoutMs completes while a peer that reads nothing is closed. Compression is gzip for accepting clients, skipped for image/video/audio/archive/woff types, and done on the io thread pool from 8 KiB up unless its threads are held by blocking work, then on the reactor (replies stay in request order). */
        constructor(opts?: { port?: number; host?: string; idleTimeoutMs?: number; maxConns?: number; compress?: boolean; metrics?: boolean;
            /** listen(2) backlog; integer 1..65535, default 1024; the kernel caps it at somaxconn (macOS 128). */
            backlog?: number;
            /** Validated (1..64) and echoed for config parity with HTTPServer; App is single-reactor, so it sizes nothing. Default 1. */
            workers?: number; });
        /** Registers a strict JSON-RPC 2.0 endpoint (typed route: bypasses use()). */
        rpc(path: string, methods: Record<string, (...args: unknown[]) => unknown>): this;
        // An rpc handler may return a ByteSource or a ReadableStream (or { stream, contentType?, status?, headers? }; a locked ReadableStream is a 500) instead of JSON: the reply is then chunked, outside the JSON-RPC envelope.

        /** Serves a document root at a URL prefix; exact and dynamic routes win over the mount, whichever was registered first. `allow` lists extensions or media types (else 403); maxFileSize defaults to 32 MiB (413); `..` is 403, dotfiles 404, the path is not percent-decoded. Symlinks resolve only while they stay under the root: a link (or `..` inside a link target) that leaves it is 403, a dangling or looping one 404. The root is reopened by path when it is renamed or replaced, so the new directory serves at once. Each open walks the path one component at a time with O_NOFOLLOW, plus a realpath containment check. */
        static(prefix: string, root: import("dyna:file").Path, opts?: { maxFileSize?: number; allow?: string[] }): this;
        /** Proxies a URL prefix to a host/port; exact and dynamic routes win over the mount. */
        proxy(prefix: string, opts: { host?: string; port?: number }): this;
        /** Upload endpoint: the handler gets the saved path and {size, contentType}; a non-matching declared Content-Type is 415, a body over maxFileSize (default 16 MiB) is 413; a refusal is sent with Connection: close while the unread body is drained (at most 64 MiB or 5 s, whatever idleTimeoutMs is), so the client can read it.
         *  The client gets `{ok: true, id}` (id = the saved file's basename, never the server path), or the handler's returned object as JSON. */
        upload(path: string, opts: { dir?: import("dyna:file").Path; maxFileSize?: number; allow?: string[] }, handler: (savedPath: string, meta: { size: number; contentType: string }) => object | void): this;
        /** WebSocket endpoint. `upgrade` runs BEFORE the 101 with origin/host/cookie/protocol; a falsy return or throw answers 403 — check `origin` when cookies authenticate. */
        ws(path: string, handlers: { upgrade?: (req: { origin: string | null; host: string | null; cookie: string | null; protocol: string | null }) => unknown; open?: (socket: WsConn) => void; message?: (socket: WsConn, data: string | Uint8Array, isBinary: boolean) => void; close?: (socket: WsConn, code: number, reason: string) => void }): this;
        /** Server-sent events endpoint; open and close receive the stream. */
        sse(path: string, handlers: { open?: (stream: SseConn) => void; close?: (stream: SseConn) => void }): this;
        /** Dynamic route for one method. Pattern segments: literal, `:param` (one decoded segment) or a trailing `*rest`; first registered wins; a method mismatch is 405.
         *  The return is the response: string, bytes, the strict envelope {status?, body?, contentType?, headers?} (header values: string or string[]; server-owned names are a 500), or any value as JSON; thenables are awaited; a throw is a 500. */
        get(pattern: string, handler: (req: AppRequest) => unknown): this;
        /**: `get` for POST. */
        post(pattern: string, handler: (req: AppRequest) => unknown): this;
        /**: `get` for PUT. */
        put(pattern: string, handler: (req: AppRequest) => unknown): this;
        /**: `get` for PATCH. */
        patch(pattern: string, handler: (req: AppRequest) => unknown): this;
        /**: `get` for DELETE (the method name cannot be a keyword). */
        del(pattern: string, handler: (req: AppRequest) => unknown): this;
        /** Appends SYNC middleware run before matched dynamic handlers with the same AppRequest; returning { response } short-circuits. Typed routes (rpc/static/ws/...) bypass it. */
        use(fn: (req: AppRequest) => { response?: unknown } | void): this;
        /** Binds the listener and starts serving; the App holds the event loop from here until close(), not from construction. A started App stays alive until close(), as in node, even when its handle is dropped (a GC root from start() to close()); an unstarted or closed one is collectable. */
        start(): void;
        /** The bound port (the real one when constructed with port 0). */
        get port(): number;
        /** The ctor's workers option (default 1); echoed only. */
        get workers(): number;
        /** The effective listen(2) backlog: the ctor option, or 1024. */
        get backlog(): number;
        close(): void;
        dispose(): void;
        readonly closed: boolean;
        readonly [Symbol.dispose]: () => void;
    }

    /** Server-side WebSocket connection, handed to App.ws handlers. */
    interface WsConn {
        send(data: BytesInput): void;
        close(): void;
        /** Open: bytes queued on the socket, including framing and the handshake response (as Node ws bufferSize); a send passing 4 MB closes the connection.
         *  Closed: starts at 0; each later send adds its data's byte length, never sent (WHATWG WebSocket send()). */
        readonly bufferedAmount: number;
    }

    /** Server-side SSE stream, handed to the App.sse open handler. */
    interface SseConn {
        send(data: string): void;
        close(): void;
        /** As WsConn.bufferedAmount (SSE has no standard): open, bytes queued on the socket with event framing, past 4 MB closes;
         *  closed, starts at 0 and each later send adds its data's byte length, never sent. */
        readonly bufferedAmount: number;
    }

    /** The request context handed to dynamic-route handlers and `app.use` middleware. */
    interface AppRequest {
        /** The method as sent; matching is exact and case-sensitive (HEAD is not aliased to GET). */
        method: string;
        /** The raw request path (percent-encoding as received), query stripped. */
        path: string;
        /** `:param` and `*rest` captures, percent-decoded; {} when the pattern has none. */
        params: Record<string, string>;
        /** The query string, percent-decoded (`+` is a space); a bare key is "", a repeated key keeps the LAST value. */
        query: Record<string, string>;
        /** Headers with lower-cased names; list fields join with ", ", Cookie with "; "; any other duplicate is a 400 before dispatch. */
        headers: Record<string, string>;
        /** The raw body as a string; the whole request is bounded at 1 MiB (body 413, headers 431, target 414). */
        body: string;
    }

    /** A WebSocket client. */
    class WsClient implements DynResource {
        /** Connects to the URL and dispatches open/message/close to the handlers. */
        constructor(url: string, handlers: { open?: (ws: WsClient) => void; message?: (ws: WsClient, data: string | Uint8Array, isBinary: boolean) => void; close?: (ws: WsClient, code: number, reason: string) => void });
        /** Sends a text frame (string) or binary frame (bytes). A closed connection silently drops the frame. Frames sent from an open/message handler leave together in one socket write when that handler returns (frames sent from a timer or promise go out at once); close() still sends every earlier frame first. */
        send(data: BytesInput): void;
        /** Performs polite teardown: a close frame first, then the connection. */
        close(): void;
        /** Performs polite teardown: a close frame first, then the connection. */
        dispose(): void;
        readonly closed: boolean;
        readonly [Symbol.dispose]: () => void;
    }

    /** RFC 6265 cookie store (src/core/httpk), shared by HTTPClient.useCookies, fetch's `cookieJar` and Fetcher's `cookieJar`. Storage model RFC 6265 5.3: host-only unless a Domain attribute domain-matches the request host (a single-label Domain is ignored; there is no public-suffix list, so Domain=co.uk from a.b.co.uk is accepted); default path is the request path's directory; Secure cookies only from and to https; __Secure-/__Host- prefixes enforced (RFC 6265bis 4.1.3); Max-Age wins over Expires, Max-Age<=0 deletes; cookies of 4096+ name+value bytes are ignored; when full, the oldest-created cookie goes. In memory only, per jar; not thread-shared. */
    class CookieJar implements DynResource {
        /** maxCookies 1..1000000, default 3000 (RFC 6265 6.1's minimum). */
        constructor(opts?: { maxCookies?: number });
        /** Applies one Set-Cookie line as if `url` had sent it; a url that is not http(s) or holds a NUL, or a line with a CTL other than HTAB (RFC 6265bis 5.6 step 1), stores nothing ("ignored"). */
        setCookie(url: string, setCookie: string): "stored" | "deleted" | "ignored";
        /** The Cookie header value for a request to `url` ("" when none or the url holds a NUL): longer paths first, then older cookies (RFC 6265 5.4). */
        cookieHeader(url: string): string;
        /** Stores every Set-Cookie line of a response headers object (HTTPResponse.headers joins them with "\n"); returns how many were stored (0 for a url holding a NUL).
         * Throws what a getter or Proxy trap of `headers` throws, and on out of memory; lines stored before the throw stay stored. */
        storeResponse(url: string, headers: Record<string, string>): number;
        clear(): void;
        /** Live cookies (expired ones are purged first). */
        readonly size: number;
        close(): void;
        dispose(): void;
        readonly closed: boolean;
        readonly [Symbol.dispose]: () => void;
    }

    /** WHATWG fetch subset (no credentials/integrity keys). It shares HTTPClient's transport and connection pool, so by default at most 64 exchanges per origin are in flight at once per thread and further requests wait FIFO for a slot (see HTTPClient; init.dyna.maxPerOrigin changes the cap). Follows at most 20 redirects unless `redirect` is "manual" (the 3xx is returned) or "error" (rejects); the redirect rule is httpk's (fetch standard 4.4): only 301/302/303/307/308 with a Location redirect; 301/302 turn only POST into GET and 303 turns every method but HEAD into GET, dropping the body and its Content-Type/-Encoding/-Language/-Location; authorization/cookie headers are dropped when a hop leaves the origin (scheme, host and port); with a `cookieJar` each hop sends that jar's cookies for its own url and stores the hop's Set-Cookie. An exchange runs on the shared reactor and holds no io pool thread (only the name lookup uses the pool). Connections are kept alive: up to 256 idle ones wait 4 s for the next request to the same host and are probed before reuse; a request that dies on a reused connection before any response byte is retried once on a fresh one only for GET/HEAD/OPTIONS, with an Idempotency-Key header, or when the kernel still holds every byte of it unacknowledged (the peer had closed before it arrived, so it was never read; TCP on macOS and Linux), and a host seen dropping one gets fresh connections for other methods for 60 s; idle ones never keep the process running. */
    function fetch(input: string | Request, init?: RequestInit): Promise<Response>;
    /** The web-platform classes, the same objects as the globals. */
    const Request: RequestConstructor;
    const Response: ResponseConstructor;
    const Headers: HeadersConstructor;
    const AbortController: AbortControllerConstructor;
    const AbortSignal: AbortSignalConstructor;
    const FormData: FormDataConstructor;
}

/** dyna:net — TCP/UDP/unix sockets (with TLS), frame decoders, Happy Eyeballs connect and a token-bucket rate limiter.
 *  B1e clean break: HTTP (dyna:http), DNS (dyna:dns), the L4 proxy (dyna:proxy), addresses (dyna:netip) and metrics (dyna:metrics) are their own modules; dyna:net re-exports none of them. Network replies are untrusted: every decoder bounds what a peer can make it allocate. */
declare module "dyna:net" {
    /** Sockets: TCP servers and clients, UDP, frame decoders, connectHappy and RateLimiter. */

    /* ---- rate limiting ---- */
    /** A token bucket over a fixed-size, open-addressed table; the table grows only with `grow: true`. Keys are hashed with SipHash-1-3 under a random per-instance key, so keys cannot be crafted to collide into one probe cluster. */
    class RateLimiter {
        /** Token-bucket table keyed by string: `tokensPerSec` (alias `refill`) is required, `burst` (alias `capacity`) defaults to one second; 1024 fixed slots unless `grow: true`. */
        constructor(opts: { tokensPerSec?: number; refill?: number; burst?: number; capacity?: number; slots?: number; grow?: boolean });
        /** True when the key may proceed, consuming `cost` tokens (default 1); cost must be > 0. */
        allow(key: string, cost?: number): boolean;
        /** The key's current token count. */
        tokens(key: string): number;
        /** Clears one key, or the whole table when no key is given. */
        reset(key?: string): void;
        /** Running totals and table geometry. */
        readonly stats: { allowed: number; denied: number; slots: number; live: number; grew: number; tokensPerSec: number; burst: number };
    }


    /* ---- frame decoders ---- */
    /** A frame decoder spec from LengthFieldDecoder, DelimiterDecoder or FixedLengthDecoder. It holds configuration only: each
     *  connection it is attached to (the decoder option of the TCPServer constructor or TCPServer.connect, or TCPConn.setDecoder)
     *  gets its own decoder state, so one spec serves any number of connections. While a connection has a decoder, every received
     *  byte goes through it: the frame handler fires once per frame with a fresh Uint8Array (its own ArrayBuffer), never once per
     *  read, and the data handler does not fire. TLS connections decode the plaintext.
     *
     *  DEFAULT = Netty 4.1 (LengthFieldBasedFrameDecoder, DelimiterBasedFrameDecoder, FixedLengthFrameDecoder), read by read:
     *  - a decoder error is reported, not fatal: the error handler gets "frame decoder: " + Netty's exception message (e.g.
     *    "Adjusted frame length exceeds 1024: 2000 - discarded"), the offending bytes are skipped as Netty skips them (a too-long
     *    frame is discarded whole, also across later reads; a negative or too-short length field skips the header only) and the
     *    connection stays open. With no error handler the bytes are dropped silently.
     *  - failFast (default true, as Netty): report a too-long frame as soon as it is detected; false: once it has been discarded.
     *  - as in Netty's ByteToMessageDecoder, an error thrown by the decoder ends the decoding of the current read: frames after it
     *    in the same read are decoded when the next read arrives, or at the peer's FIN (where a second error drops the rest).
     *    LineBasedFrameDecoder fires its errors without throwing, so it keeps decoding the same read.
     *  - DelimiterDecoder with exactly the two delimiters "\r\n" and "\n" (either order) runs Netty's LineBasedFrameDecoder, as
     *    Netty's DelimiterBasedFrameDecoder does: a line is too long when everything buffered without a newline (a trailing "\r"
     *    included) exceeds maxFrameLength, and its messages are "frame length (N) exceeds the allowed maximum (M)".
     *  - other delimiter sets: the delimiter giving the shortest frame wins, a tie goes to the earlier one in the list; a trailing
     *    partial delimiter is not counted against maxFrameLength (Netty 4.1 partialDelimiterLength), so a max-length frame whose
     *    delimiter is split across reads is delivered; overlapping sets (e.g. ["bb", "b"]) decide on what has arrived, so "1b" then
     *    "b2b" gives "1", "", "2" while "1bb2b" in one read gives "1", "2" (holdOverlapping changes that).
     *  - LengthField width 8 reads a signed long: a length with the top bit set is "negative pre-adjustment length field: N"; the
     *    adjusted length (raw + adjustment + offset + width, Java long arithmetic, so it can wrap negative) decides "too short" or
     *    "too long". A length above 2^53 is never usable as a frame size here (maxFrameLength <= 2^53), so after adjustment it is
     *    necessarily too long unless the adjustment brings it back into range (then it is a valid frame, as in Netty).
     *  - at the peer's FIN the frames already decoded are delivered, decoding resumes on what an error left buffered, then the
     *    connection closes; an incomplete trailing frame is dropped without an error (as Netty): a truncated stream and a clean
     *    close look alike, so a protocol that must tell them apart frames an explicit end marker. A TLS close_notify ends the
     *    stream the same way.
     *  Known deviations from Netty 4.1 in the default mode: none (checked against a literal per-read port of the 4.1 decoders).
     *
     *  EXTENSION beyond Netty (dyna-specific, the `meta` argument, all off by default): stopOnError, rejectUnsafeLength,
     *  holdOverlapping, resumeAfterError (see the meta interfaces). holdOverlapping + stopOnError makes a DelimiterDecoder's frames and final error
     *  independent of how TCP segmented the stream; stopOnError makes a LengthFieldDecoder's independent of it. Without
     *  stopOnError, Netty's error handling above (decoding ends with the read) keeps the events after an error read-dependent.
     *
     *  pause() inside a frame handler stops delivery at once: frames already decoded from the same read wait in the connection and
     *  resume() delivers them before reading again, so none is lost. On the io_uring backend pause() is a no-op (see TCPConn.pause).
     *  A frame spanning reads is copied once into the decoder's buffer and once into its Uint8Array; a frame inside one read is
     *  copied once. */
    interface FrameDecoder {
        readonly kind: "length" | "delimiter" | "fixed";
        /** The largest frame accepted; for "fixed", the frame length. */
        readonly maxFrameLength: number;
    }
    /** LengthFieldDecoder options: Netty's constructor parameters (a strict bag: an unknown key is a TypeError). */
    interface LengthFieldDecoderOptions {
        /** Bytes before the length field, 0..2^53; default 0 (lengthFieldOffset). */
        offset?: number;
        /** Length field size in bytes: 1, 2, 3, 4 or 8 (RangeError otherwise); default 4. 1..4 are unsigned, 8 is a signed long. */
        width?: 1 | 2 | 3 | 4 | 8;
        /** Byte order of the length field; default "be". */
        endian?: "be" | "le";
        /** lengthAdjustment: frameLength = raw + adjustment + offset + width. |adjustment| <= 2^53; default 0. */
        adjustment?: number;
        /** Bytes dropped from the front of each frame, 0..2^53; more than the frame length is an error (the frame is skipped); default 0. */
        initialBytesToStrip?: number;
        /** The largest whole frame (header included, before stripping), 1..2^53, at least offset + width (RangeError otherwise);
         *  default 1048576. */
        maxFrameLength?: number;
        /** Netty failFast; default true. */
        failFast?: boolean;
    }
    /** dyna-specific extension for LengthFieldDecoder (not in Netty; a strict bag, all default false). */
    interface LengthFieldDecoderMeta {
        /** The first error is fatal: it is reported (at detection, whatever failFast says), nothing after it is decoded, and the
         *  connection closes once the frames decoded before it are delivered. */
        stopOnError?: boolean;
        /** A raw length field above 2^53 (including width-8 values Netty reads as negative) is its own error, "length field N
         *  exceeds 2^53 (rejectUnsafeLength)", instead of Netty's signed-long arithmetic. */
        rejectUnsafeLength?: boolean;
        /** Decode the bytes left in the read right after an error instead of waiting for the next read or end of input (Netty's
         *  callDecode stops at a thrown TooLongFrameException, so by default the bytes after an error wait, which can deadlock a
         *  request/response peer that sends nothing more). The errors and frames are those of Netty's decode with the stop removed.
         *  Ignored under stopOnError. */
        resumeAfterError?: boolean;
    }
    /** DelimiterDecoder options: Netty's constructor parameters (a strict bag). */
    interface DelimiterDecoderOptions {
        /** One delimiter or a list of 1..64; each is 1..4096 bytes (a string is UTF-8). Required. */
        delimiter: BytesInput | BytesInput[];
        /** Drop the delimiter from the delivered frame; default true. */
        stripDelimiter?: boolean;
        /** The largest frame without its delimiter, 1..2^53; default 1048576. */
        maxFrameLength?: number;
        /** Netty failFast; default true. */
        failFast?: boolean;
    }
    /** dyna-specific extension for DelimiterDecoder (not in Netty; a strict bag, all default false). */
    interface DelimiterDecoderMeta {
        /** The first error is fatal (reported at detection), then the connection closes; see LengthFieldDecoderMeta.stopOnError. */
        stopOnError?: boolean;
        /** For delimiter sets where one delimiter can begin inside another (e.g. ["bb", "b"]): a frame is delivered only when no
         *  longer delimiter still arriving could win, so frames equal Netty on the whole stream at once; the peer's FIN resolves a
         *  held frame (as Netty's decodeLast would on the coalesced stream). Turns off the LineBasedFrameDecoder path. */
        holdOverlapping?: boolean;
        /** See LengthFieldDecoderMeta.resumeAfterError. */
        resumeAfterError?: boolean;
    }
    /** Length-prefixed frames (Netty LengthFieldBasedFrameDecoder). A plain function: `new` is a TypeError. */
    function LengthFieldDecoder(opts?: LengthFieldDecoderOptions, meta?: LengthFieldDecoderMeta): FrameDecoder;
    /** Delimiter-terminated frames (Netty DelimiterBasedFrameDecoder / LineBasedFrameDecoder). A plain function. */
    function DelimiterDecoder(opts: DelimiterDecoderOptions, meta?: DelimiterDecoderMeta): FrameDecoder;
    /** Every `length` bytes is a frame, 1..2^53 (Netty FixedLengthFrameDecoder). A plain function. */
    function FixedLengthDecoder(length: number): FrameDecoder;
    /* ---- sockets and proxying ---- */
    /** A TCP connection handed to a handler; write is refused on a TLS connection before the handshake, and refused with a TypeError past the connection's high-water mark. */
    interface TCPConn {
        write(data: BytesInput): void;
        /** Bytes queued for this connection that the kernel has not taken (0 on a closed connection). */
        readonly bufferedAmount: number;
        close(): void;
        /** Stops reading: the receive is cancelled, so no data event fires and the kernel's buffer fills until the peer's sends block (TCP backpressure). A FIN or reset is not seen until resume().
         *  A paused connection is exempt from idleTimeoutMs. Idempotent; a no-op on a closed connection; a TypeError on a TLS connection before its handshake completed.
         *  On the opt-in io_uring backend (IO_URING=1 builds) pause() is a no-op: cancelling a multishot receive there drops a completion already queued, so streams() has no read backpressure on that backend. */
        pause(): void;
        /** Re-arms reading; on TLS it first delivers plaintext already decrypted when the pause landed. Idempotent; a no-op on a closed connection. */
        resume(): void;
        /** True between pause() and resume() on an open connection. */
        readonly paused: boolean;
        /** Half-close: shutdown(SHUT_WR) once nothing is queued (a TypeError while bytes are queued: wait for drain). The peer reads EOF; this side can still read. TLS too (since v10b-26a; it used to close() the socket, and a close() with unread bytes in the receive buffer, such as a late TLS 1.3 session ticket, sends RST, which drops the ciphertext still in the send buffer: the peer lost the tail of a stream whose writer.close() had resolved). No close_notify is sent: the TLS stream ends with the TCP FIN. */
        end(): void;
        /** WHATWG streams over THIS connection; once called, its data and drain events go to the streams instead of the TCPHandlers (connect/close still fire). Call it from the connect handler. Throws when called twice or on a closed connection.
         *  readable: a byte stream; each received chunk is enqueued and the connection pause()s once desiredSize <= 0, and a pull resume()s it, so a slow reader pushes back on the peer. cancel() stops reading (the connection is paused).
         *  writable: a chunk (BufferSource or string) is written in pieces of at most min(writableHighWaterMark, the connection's highWaterMark) bytes, each waiting for drain while bufferedAmount would pass that size, so a write never hits the high-water refusal; a write resolves once its last piece is queued.
         *  close() waits for the queue to drain, then end()s; abort() closes the connection. A failed send closes the connection and errors the writable ("the connection closed").
         *  The peer's FIN closes the readable; without allowHalfOpen the connection then closes once its queue drains, with it this side keeps writing until writable.close(). The connection closes when the readable is done (EOF or cancel) and the writable is closed.
         *  A close of the connection (peer reset, close(), idle timeout) closes the readable and errors the writable; closing the TCPServer itself drops its connections without events, so their streams stay pending.
         *  streams() is byte-mode only: on a connection with a frame decoder it is a TypeError (frames go to the frame handler). */
        /** Attaches a frame decoder to THIS connection (see FrameDecoder); from then on its bytes go to the frame handler, starting with
         *  any bytes the kernel already holds (pause() in the connect handler, setDecoder, then resume() to switch mid-stream without
         *  a data event). One decoder per connection for its lifetime. TypeError when the connection is closed, when dec is not a
         *  FrameDecoder, when a decoder is already set, after streams(), or when the handlers have no frame function.
         *  connectHappy takes no decoder option: call setDecoder from its connect handler. */
        setDecoder(dec: FrameDecoder): void;
        streams(opts?: TCPStreamsOptions): { readable: ReadableStream<Uint8Array>; writable: WritableStream<ArrayBufferView | ArrayBuffer | string> };
    }
    /** Options for TCPConn.streams(). */
    interface TCPStreamsOptions {
        /** Readable queue size in bytes before the connection pauses; default 65536 (0 = pause after every chunk until a read). */
        readableHighWaterMark?: number;
        /** Largest piece written at once and the queued-bytes level a write waits below, 1..2^30; default 65536, capped at the connection's highWaterMark. */
        writableHighWaterMark?: number;
        /** Keep the connection open for writing after the peer's FIN (default false: close once the queue drains). */
        allowHalfOpen?: boolean;
    }
    /** TCP server and client event handlers. */
    interface TCPHandlers {
        /** Fires when a connection lands; on failure conn is null and err names the reason. */
        connect?: (conn: TCPConn | null, err: string | null) => void;
        /** Fires with a copy of the received bytes. */
        data?: (conn: TCPConn, bytes: Uint8Array) => void;
        /** Fires exactly once per emptying of the connection's outbound queue after a write left bytes queued; the producer's signal to resume. */
        drain?: (conn: TCPConn) => void;
        close?: (conn: TCPConn) => void;
        /** Fires once per frame while the connection has a decoder (in place of data), with a fresh Uint8Array. Required when a decoder is set. */
        frame?: (conn: TCPConn, frame: Uint8Array) => void;
        /** Fires once per decoder error ("frame decoder: <Netty's message>"), in order with the frames: after the frames decoded before it. By default the connection stays open and decoding goes on (Netty); with the decoder's stopOnError meta option the connection closes after it. */
        error?: (conn: TCPConn, message: string) => void;
    }
    /** Server TLS: cert/key required; requestCert turns on mTLS (needs ca). */
    /** Server TLS: cert/key/ca are PEM file paths (string or Path). */
    interface TCPServerTLSOptions {
        cert: string | import("dyna:file").Path;
        key: string | import("dyna:file").Path;
        alpn?: string;
        ca?: string | import("dyna:file").Path;
        requestCert?: boolean;
    }
    /** Client TLS: the verified name defaults to host; cert/key give mTLS; `ca` pins a CA; rejectUnauthorized:false disables verification. */
    interface TCPClientTLSOptions {
        ca?: string | import("dyna:file").Path;
        servername?: string;
        alpn?: string | string[];
        minVersion?: string;
        rejectUnauthorized?: boolean;
        cert?: string | import("dyna:file").Path;
        key?: string | import("dyna:file").Path;
    }
    /** Options for TCPServer.connect; with a unix-socket `path`, TLS needs an explicit servername or rejectUnauthorized:false. */
    interface TCPConnectOptions {
        host?: string;
        port?: number;
        path?: string;
        connectTimeoutMs?: number;
        maxConnections?: number;
        idleTimeoutMs?: number;
        tls?: boolean | TCPClientTLSOptions;
        /** Outbound queue cap per connection in bytes (1..2^30); a write past it is refused with a TypeError until drain fires. */
        highWaterMark?: number;
        /** Frame decoder for the connection (see FrameDecoder); the handlers then need a frame function (TypeError otherwise). */
        decoder?: FrameDecoder;
    }
    /** A TCP server, and (via the static connect) a TCP client; runs on the shared io reactor. */
    class TCPServer implements DynResource {
        constructor(opts?: { port?: number; path?: string; maxConnections?: number; idleTimeoutMs?: number; tls?: TCPServerTLSOptions; /** outbound queue cap per connection in bytes (1..2^30), default 4 MiB */ highWaterMark?: number; /** frame decoder for every accepted connection (each gets its own state); start() then needs a frame handler */ decoder?: FrameDecoder });
        /** Connects to a peer and returns the client resource; events arrive through TCPHandlers. The client lives while connecting or connected even if the handle is dropped; once its connection ends it no longer holds the event loop. */
        static connect(opts: TCPConnectOptions, handlers?: TCPHandlers): TCPServer;
        /** Binds and accepts; a port of 0 resolves into `.port`. A started server (TCP, TLS or IPC) stays alive until close(), as in node, even when its handle is dropped: it is a GC root from start() to close() and keeps accepting; an unstarted or closed server is collectable. */
        start(handlers?: TCPHandlers): void;
        /** The bound port. */
        readonly port: number;
        close(): void;
        dispose(): void;
        readonly closed: boolean;
        readonly [Symbol.dispose]: () => void;
    }

    /** A bound UDP socket; binds in the constructor. `host` defaults to every IPv4 interface; a host containing ':' ("::", "::1", "fe80::1%lo0") binds an IPv6 socket, dual-stack (IPV6_V6ONLY off), so one bound to "::" also hears IPv4 peers as "::ffff:a.b.c.d". */
    class UDPSocket implements DynResource {
        constructor(opts?: { port?: number; host?: string });
        /** Arms the receive path; `message(data, from)` receives a copy of each datagram; `from.address` is numeric ("127.0.0.1", "::1", "::ffff:127.0.0.1") and passes straight back to send(). A started socket stays alive until close(), as in node, even when its handle is dropped (a GC root from start() to close()); an unstarted or closed one is collectable. */
        start(handlers?: { message?: (data: Uint8Array, from: { address: string; port: number }) => void }): void;
        /** Sends a datagram to host:port, where host is an IPv4 or IPv6 literal (no name lookup); an IPv6 host needs a socket bound to an IPv6 host (TypeError otherwise), and such a socket sends to IPv4 literals v4-mapped. Returns the bytes sent. A failed sendto(2) throws an InternalError "UDPSocket: send failed: <strerror> (<CODE>)" carrying .errno and .code (EMSGSIZE for a datagram over the OS limit, 65507 bytes on IPv4 and net.inet.udp.maxdgram, default 9216, on macOS; EAGAIN/ENOBUFS when the send buffer is full: the datagram is dropped, not queued; EHOSTUNREACH/ENETUNREACH; EPIPE, which macOS can return for a send to the host's own LAN address while the Application Firewall blocks all incoming connections). A failed send does not close or wedge the socket: later sends and the receive path keep working. */
        send(data: BytesInput, host: string, port: number): number;
        /** The bound port. Port 0 takes an ephemeral port no other socket holds; an explicit port binds with SO_REUSEADDR. On Linux a UDP socket with SO_REUSEADDR does not count as holding its port during ephemeral selection, so a port-0 socket bound that way could be handed a live socket's port and take its unicast traffic. */
        readonly port: number;
        close(): void;
        dispose(): void;
        readonly closed: boolean;
        readonly [Symbol.dispose]: () => void;
    }

    /** Happy Eyeballs (simplified RFC 8305): both address families race, first success wins; fallbackMs is the whole-race deadline, counted from the end of name resolution. A host name is resolved off the event loop, so the call returns at once and a failed lookup reaches the `connect` handler as "connectHappy: DNS resolution failed"; an address literal is used as is. `tls` options are required for TLS endpoints. */
    function connectHappy(host: string, port: number, opts?: { fallbackMs?: number; tls?: boolean | TCPClientTLSOptions }, handlers?: TCPHandlers): TCPServer;

    /** Process-wide count of throws swallowed from net event handlers (TCP connect/data/close/drain, UDP message, DNSResolver query callback): the only diagnostic for them. */
    function swallowedHandlerThrows(): number;
}


/** dyna:netip — IP addresses and CIDR prefixes: parsing, RFC 5952 canonical text, classification and containment. Pure functions, no IO.
 *  Split out of dyna:net as a clean break (B1e). */
declare module "dyna:netip" {
    /** A parsed IP address. */
    interface ParsedAddr {
        readonly is4: boolean;
        readonly is6: boolean;
        readonly bytes: Uint8Array;
        readonly string: string;
    }
    /** A parsed CIDR prefix. */
    interface ParsedPrefix {
        readonly addr: string;
        readonly bits: number;
    }
    /** Parses an IPv4/IPv6 address (a 4-in-6 form parses as IPv6); throws TypeError on a malformed address. */
    function parseAddr(addr: string): ParsedAddr;
    /** Parses "addr/bits" into {addr, bits}; the addr is formatted canonically; throws TypeError on a malformed prefix. */
    function parsePrefix(cidr: string): ParsedPrefix;
    /** True when the prefix contains the address; throws TypeError when either argument is malformed. */
    function contains(prefix: string, addr: string): boolean;
    /** The prefix's network address (host bits zeroed), canonically formatted; throws TypeError on a malformed prefix. */
    function masked(cidr: string): string;
    /** The RFC 5952 canonical text of an address; a 4-in-6 address formats as "::ffff:a.b.c.d". */
    function canonical(addr: string): string;
    /** True when the string parses as an IP address; never throws. */
    function isValid(addr: string): boolean;
    /** Total order on addresses: -1, 0, or 1; IPv4 sorts before IPv6. */
    function compareAddr(a: string, b: string): -1 | 0 | 1;
    /** True for 127.0.0.0/8 and ::1. */
    function isLoopback(addr: string): boolean;
    /** True for RFC 1918 space and fc00::/7. */
    function isPrivate(addr: string): boolean;
    /** True for 224.0.0.0/4 and ff00::/8. */
    function isMulticast(addr: string): boolean;
    /** True for 0.0.0.0 and ::. */
    function isUnspecified(addr: string): boolean;
    /** True for 169.254.0.0/16 and fe80::/10. */
    function isLinkLocalUnicast(addr: string): boolean;
    /** True for a global unicast address (not loopback, multicast, link-local or unspecified). */
    function isGlobalUnicast(addr: string): boolean;
    /** True for 224.0.0.0/24 and ff02::/16. */
    function isLinkLocalMulticast(addr: string): boolean;

    /** A compiled CIDR prefix (contains/overlaps/masked/bits), parsed and masked once; unrelated to dyna:structures' string Trie. */
    class Prefix {
        constructor(cidr: string);
        /** True when the address is inside the prefix; an unparseable address is false, not an error. */
        contains(addr: string): boolean;
        /** True when the two prefixes share any address; different families never overlap. */
        overlaps(other: Prefix): boolean;
        /** The network address, canonically formatted. */
        readonly masked: string;
        /** The prefix length. */
        readonly bits: number;
        /** True when this is an IPv4 prefix. */
        readonly isIPv4: boolean;
    }

}

/** dyna:dns — a UDP DNS client (resolver) and server. Answers are untrusted: every decoder bounds what a peer can make it allocate.
 *  Split out of dyna:net as a clean break (B1e). */
declare module "dyna:dns" {
    /** One decoded answer record: `name`, `type` and `ttl` are always present; the payload field depends on the type (A/AAAA -> address, CNAME/NS/PTR -> target, MX -> priority+exchange, TXT -> chunks). */
    interface DNSRecord {
        readonly name: string;
        readonly type: number;
        readonly ttl: number;
        readonly address?: string;
        readonly target?: string;
        readonly priority?: number;
        readonly exchange?: string;
        readonly chunks?: string[];
    }
    /** A UDP DNS client; answers are matched by connected socket, CSPRNG query ID and echoed question.
        A GC root while any query is in flight (a dropped handle still settles); collectable once idle. */
    class DNSResolver implements DynResource {
        /** timeoutMs must fit uint32 (else RangeError). `{ttl}` seconds (at most 86400) enables the answer cache: hits return a fresh deep copy; no single-flight. */
        constructor(opts?: { server?: string; port?: number; timeoutMs?: number; /** answer-cache cap in seconds (1..86400); absent/0 disables caching */ ttl?: number });
        /** Queries a name; `type` is the record type (A is 1). The callback receives (err, records). */
        query(name: string, type: number, callback?: (err: string | null, records: DNSRecord[] | undefined) => void): void;
        /** Resolves decoded answers (type defaults to A), following a CNAME chain inside the answer (8 hops): records are the final name's of `type`; a trailing callback gets (err, records). */
        lookup(name: string, callback?: (err: string | null, records: DNSRecord[] | undefined) => void): Promise<DNSRecord[]> | undefined;
        lookup(name: string, type: number, callback?: (err: string | null, records: DNSRecord[] | undefined) => void): Promise<DNSRecord[]> | undefined;
        /** Sugar over lookup(name, 5): the chain targets. */
        resolveCname(name: string): Promise<string[]>;
        /** Sugar over lookup(name, 15): {priority, exchange} pairs. */
        resolveMx(name: string): Promise<{ priority: number; exchange: string }[]>;
        /** Sugar over lookup(name, 16): one chunk array per record. */
        resolveTxt(name: string): Promise<string[][]>;
        /** Sugar over lookup(name, 2): the nameservers. */
        resolveNs(name: string): Promise<string[]>;
        close(): void;
        dispose(): void;
        readonly closed: boolean;
        readonly [Symbol.dispose]: () => void;
    }

    /** A UDP DNS server with an amplification cap and a per-source token bucket. */
    class DNSServer implements DynResource {
        constructor(opts?: { port?: number; host?: string });
        /** Starts answering; the handler maps (name, type) to an address string, or null for no answer. A started server stays alive until close(), as in node, even when its handle is dropped (a GC root from start() to close()); an unstarted or closed one is collectable. */
        start(handler: (name: string, type: number) => string | null): void;
        /** The bound port (resolved when constructed with port 0). */
        readonly port: number;
        close(): void;
        dispose(): void;
        readonly closed: boolean;
        readonly [Symbol.dispose]: () => void;
    }

}

/** dyna:proxy — TCPProxy, an L4 byte reverse proxy on the shared io reactor with no JS on the data path.
 *  Split out of dyna:net as a clean break (B1e). */
declare module "dyna:proxy" {
    /** One upstream entry for TCPProxy. */
    interface TCPUpstream {
        host?: string;
        port: number;
    }
    /** TCPProxy counters. */
    interface TCPProxyStats {
        readonly live: number;
        readonly accepted: number;
        readonly refused: number;
        readonly idleClosed: number;
        readonly connectFailed: number;
        readonly bytesUp: number;
        readonly bytesDown: number;
    }
    /** An L4 byte reverse proxy with no JS on the data path. Defaults: maxConns 8192, connectTimeoutMs 10000, no idle timeout, listening on 0.0.0.0. */
    class TCPProxy implements DynResource {
        constructor(opts: { port: number; host?: string; upstream: TCPUpstream | TCPUpstream[]; maxConns?: number; idleTimeoutMs?: number; connectTimeoutMs?: number });
        /** Binds and starts forwarding; a port of 0 resolves into `.port`. A started proxy stays alive until close(), as in node, even when its handle is dropped (a GC root from start() to close()); an unstarted or closed one is collectable. */
        start(): void;
        /** Live pairs and cumulative counters. */
        stats(): TCPProxyStats;
        /** The bound port. */
        readonly port: number;
        close(): void;
        dispose(): void;
        readonly closed: boolean;
        readonly [Symbol.dispose]: () => void;
    }

}

/** dyna:metrics — a process-wide registry of counters, gauges and histograms with a Prometheus text scrape; dyna:http's App records into it and serves it at /metrics.
 *  Split out of dyna:net as a clean break (B1e). */
declare module "dyna:metrics" {
    /** A registry of counters, gauges and histograms with a Prometheus text scrape; at most 256 series (RangeError beyond). */
    const Metrics: {
        /** Increments a counter; a negative or NaN increment is refused. */
        counter(name: string, value?: number, labels?: Record<string, string>): void;
        /** Sets a gauge to a value. */
        gauge(name: string, value: number, labels?: Record<string, string>): void;
        /** Records an observation; buckets are 5ms..1s by default, or 1..6 increasing `opts.buckets` edges fixed at the series' first use. */
        histogram(name: string, value: number, labels?: Record<string, string>, opts?: { buckets: number[] }): void;
        /** The registry as Prometheus text exposition. */
        scrape(): string;
        /** Empties the registry; intended for tests. */
        reset(): void;
    };

}


/** dyna:postgres — a PostgreSQL client over the extended protocol (SCRAM-SHA-256/MD5 auth, TLS, pipelining, a prepared-statement cache).
 *  Split out of dyna:net as a clean break: dyna:net no longer exports PostgreSQL. Server replies are untrusted and bounded. */
declare module "dyna:postgres" {
    /** PostgreSQL client options. */
    interface PostgreSQLOptions {
        host?: string;
        path?: string;
        port?: number;
        user?: string;
        password?: string;
        database?: string;
        applicationName?: string;
        /** Connect through TLS via SSLRequest; a server answering `N` ends the connection — there is no silent fallback to plaintext. */
        tls?: boolean;
        /** CA certificate PEM file pinned for the TLS handshake; omitted uses the system trust store. */
        ca?: string;
        raw?: boolean;
        bytes?: boolean;
        textResults?: boolean;
        /** Prepared-statement cache capacity (default 64); cached after `prepareAfter` (default 2) executions. */
        statementCacheSize?: number;
        prepareAfter?: number;
        bigint?: boolean;
        /** numeric columns: "string" (default, exact server text) or a constructor called as new C(text), e.g. Decimal from dyna:decimal; NaN/Infinity become numbers; a throwing constructor rejects the query. */
        numeric?: "string" | (new (text: string) => unknown);
        /** "string" (default) or "date": date/timestamp/timestamptz become Date (UTC; microseconds truncated to ms; outside Date range: ISO string), infinity ±Infinity,
         *  interval { months, days, microseconds } (bigint past 2^53); text results need DateStyle ISO and IntervalStyle postgres, else stay strings. JS thread: C 23/25/11 ns per int/quoted-text/binary element; end to end +0.1..0.7 µs/row vs strings (int4[10], text[5], jsonb; 1e3-1e5 rows), under the ~3.6 µs pool hop. */
        dates?: "string" | "date";
        /** json/jsonb: "string" (default, server text) or "parse" (JSON.parse on the JS thread; numbers past 2^53 lose precision; malformed JSON rejects the query). */
        json?: "string" | "parse";
        /** "string" (default, PG text such as {1,"a b",NULL}) or "array": arrays of bool/bytea/int2/4/8/float4/8/oid/text/varchar/bpchar/numeric/date/timestamp(tz)/interval/uuid/json(b) become nested arrays
         *  (lower bounds dropped, max 6 dims; malformed text stays a string); elements decode like the column type (bigint, numeric, dates, json, bytes). JS thread: C 23.1/24.6/10.6 ns per int/quoted-text/binary element, +-3.64 µs/row (int4[10], text[5], jsonb) at 1e5 rows, under the ~3.6 µs pool hop. */
        arrays?: "string" | "array";
        /** "string" (default) or "object": int4/int8/num/ts/tstz/date ranges become { empty, lower, upper, lowerInclusive, upperInclusive } (unbounded side null; bounds decode like
         *  the subtype: bigint, numeric, dates), multiranges arrays of them (empty members dropped); malformed text stays a string. JS thread: C 13.4/88.0/3.2 ns per int4/quoted-tstz/binary range; end to end within ±0.1 µs/row of strings (int4range, tstzrange, int4multirange; 1e3-1e5 rows), under the ~3.6 µs pool hop. */
        ranges?: "string" | "object";
        /** Numeric IPv4/IPv6 address(es), a comma list paired with host (host then names TLS verification and the .pgpass match; tls needs host). libpq 18 hostaddr. */
        hostaddr?: string;
        /** libpq 18 target_session_attrs (default "any"; url key, PGTARGETSESSIONATTRS): read-write/read-only check ParameterStatus default_transaction_read_only + in_hot_standby, else SHOW transaction_read_only; primary/standby check
         *  in_hot_standby, else pg_is_in_recovery() (server < 9.0 = primary); a mismatch sends Terminate and tries the next host; prefer-standby then retries the list as any. Invalid: RangeError. */
        targetSessionAttrs?: "any" | "read-write" | "read-only" | "primary" | "standby" | "prefer-standby";
        /** libpq 18 load_balance_hosts (default "disable"; url key, PGLOADBALANCEHOSTS): "random" shuffles the host list once per client (Fisher-Yates, OS-entropy seed). Invalid: RangeError. */
        loadBalanceHosts?: "disable" | "random";
        /** libpq URI postgres[ql]://user:pass@host:port/db?k=v (≤8192 bytes; bad %-escape, %00, inner space, unknown key: TypeError); options win, then url, then env. Lists per libpq 18 34.1.1.3 (≤1024 hosts, else RangeError): host/hostaddr/port comma lists, 1 or N ports, empty = default, else libpq pqParseIntParam (blanks, sign; non-integer or outside 1..65535: RangeError); tried in order, next host on refused, per-host connect_timeout or 57P03, never after an auth or TLS error; all failed: queries reject (a dead client throws) with every host's reason.
         *  Keys host port user password dbname application_name sslrootcert sslnegotiation connect_timeout(s) target_session_attrs load_balance_hosts ssl=true sslmode: disable | require/verify-ca/verify-full (= tls: true, always verified); allow/prefer throw RangeError (downgradable to plaintext). */
        url?: string | URL;
        /** Default true: unset host/port/user/password/database come from PGHOST/PGPORT/PGUSER/PGPASSWORD/PGDATABASE, then the password from PGPASSFILE or $HOME/.pgpass
         *  (libpq format; skipped unless a regular file without group/other bits, at most 1 MiB, no NUL; no host or a socket matches localhost). Read on the I/O pool before the connect (errors reject queries): open+read+match missing=1296 small=13424 lines10k=544390 ns. */
        env?: boolean;
        /** Default false: MD5/cleartext auth is refused on a plaintext connection unless true (accepted under `tls: true`). */
        insecureAuth?: boolean;
        /** Default 'prefer': SCRAM-SHA-256-PLUS with tls-server-end-point (RFC 5929) when TLS is on and the server offers it, else SCRAM-SHA-256; PLUS offered with an unusable cert hash (e.g. Ed25519) fails, as in libpq.
         *  'require' fails the connection unless PLUS is used (no TLS, no PLUS, cert hash unusable, or password/MD5/trust auth); 'disable' never binds. */
        channelBinding?: "disable" | "prefer" | "require";
        /** Highest protocol requested, default "3.0" as in libpq 18 (poolers and proxies may break on 3.2); a higher minProtocolVersion raises it. */
        maxProtocolVersion?: "3.0" | "3.2";
        /** Lowest protocol accepted, default "3.0"; a NegotiateProtocolVersion below it fails the connection. */
        minProtocolVersion?: "3.0" | "3.2";
        /** "postgres" (default): SSLRequest, then TLS; "direct": TLS at once (PostgreSQL 17+, needs tls: true, server must select ALPN "postgresql").
         *  ALPN "postgresql" is offered in both; cancel() negotiates TLS the same way (libpq 17), with no plaintext fallback. */
        sslNegotiation?: "postgres" | "direct";
        maxMessageBytes?: number;
        /** Bound on ONE result set (wire bytes plus 64 per row), default 1 GiB; past it the connection fails with "result exceeds maxResultBytes". */
        maxResultBytes?: number;
        maxPending?: number;
        queryTimeoutMs?: number;
        connectTimeoutMs?: number;
    }
    /** The prepared-statement cache's live state. */
    interface StatementCacheStats {
        readonly size: number;
        readonly max: number;
        readonly prepareAfter: number;
        readonly preparedHits: number;
        readonly unnamed: number;
    }
    /** A PostgreSQL client; parameters are bound by the extended protocol, never interpolated. */
    class PostgreSQL implements DynResource {
        /** Same as new PostgreSQL({ ...opts, url }); a url in both places throws TypeError. A host starting with / is a socket directory. */
        constructor(url: string, opts?: PostgreSQLOptions);
        constructor(opts?: PostgreSQLOptions);
        /** Runs one statement (extended protocol when params are given); `opts.maxRows` rejects that query when exceeded, leaving the connection usable. */
        query(sql: string, params?: unknown[], opts?: { maxRows?: number }): Promise<unknown>;
        /** 1..65535 statements, one round trip, one Sync (one implicit transaction). Throws TypeError/RangeError on a bad list, InternalError when closed or over maxPending.
         *  Resolves to a result or Error per statement (first failure, then a skipped Error each); rejects on connection failure, close() or out of memory. */
        pipeline(statements: [sql: string, params?: unknown[]][]): Promise<({ rows: unknown[]; fields: unknown[]; command: string; rowCount: number } | Error)[]>;
        /** Asks the server to cancel the running query over a fresh connection, TLS as for the session (no plaintext fallback); best effort, no reply. */
        cancel(): void;
        /** Registers a notice, notification or error handler; returns the client. */
        on(event: "notice" | "notification" | "error", handler: (data: unknown) => void): this;
        /** True once the startup handshake has completed. */
        readonly ready: boolean;
        /** Negotiated protocol, null until ready; the cancel key is 4 bytes in "3.0", 4..256 in "3.2". */
        readonly protocolVersion: "3.0" | "3.2" | null;
        /** The statement cache's size and how often each arm was taken. */
        readonly statementCache: StatementCacheStats;
        /** Queries issued but not yet answered. */
        readonly pending: number;
        /** The statement cache's size and how often each arm was taken. */
        readonly statementCache: StatementCacheStats;
        /** Queries issued but not yet answered. */
        readonly pending: number;
        /** The server's backend PID (valid once ready). */
        readonly backendPid: number;
        /** The transaction status character: I (idle), T (in transaction), E (failed). */
        readonly transactionStatus: string;
        /** Streams rows in constant memory. queryIter is NATIVE (a DECLARE CURSOR/FETCH FORWARD driver); a `break` always ends its transaction; {batch} rows per fetch, {maxRows} in total.
         *  Lazy: no IO until the first next(). src/pool.js no longer needs importing for this: its installer stands down when the native method exists. */
        queryIter(sql: string, params?: unknown[], opts?: { batch?: number; maxRows?: number }): AsyncIterableIterator<Record<string, unknown>>;
        /** COPY ... FROM STDIN; string/bytes are copied at call, an (async) iterable of string|Uint8Array|ArrayBuffer is pulled while the socket queue is <= highWaterMark (bytes, 1..2^30, default 1 MiB).
         *  A source throw, rejection or bad chunk sends CopyFail and rejects with that error; the connection stays usable and later queries wait for the COPY; query() on COPY FROM STDIN sends CopyFail. */
        copyFrom(sql: string, source: string | Uint8Array | ArrayBuffer | Iterable<string | Uint8Array | ArrayBuffer> | AsyncIterable<string | Uint8Array | ArrayBuffer>, opts?: { highWaterMark?: number }): Promise<unknown>;
        /** COPY ... TO STDOUT as Uint8Array chunks; receiving pauses while > highWaterMark bytes are unread (TCP/Unix, not TLS); one next() at a time; return() discards the rest; done value = query result.
         *  Throws TypeError unless sql starts with COPY; a COPY that never streams (TO file/PROGRAM) rejects next() with TypeError; query() refuses TO STDOUT; CopyBoth fails the connection. */
        copyTo(sql: string, opts?: { highWaterMark?: number }): AsyncIterableIterator<Uint8Array>;
        /** The server parameters from startup (server_version, client_encoding, ...). */
        readonly parameters: Record<string, string>;
        close(): void;
        dispose(): void;
        readonly closed: boolean;
        readonly [Symbol.dispose]: () => void;
    }

    // PgPool/RedisPool are NOT exports of dyna:postgres or dyna:redis: they are pure-JS classes imported by file path (see the src/pool.js block).
}

/** dyna:redis — a Redis client (RESP2/RESP3, TLS, pub/sub, pipelined FIFO commands).
 *  Split out of dyna:net as a clean break: dyna:net no longer exports Redis. Replies are untrusted: every decoder bounds what a peer can make it allocate. */
declare module "dyna:redis" {
    /** Redis client options. */
    interface RedisOptions {
        host?: string;
        path?: string;
        port?: number;
        db?: number;
        username?: string;
        password?: string;
        /** Connect through TLS; certificates are verified against the system trust store with hostname checking (`ca` pins a CA PEM file). */
        tls?: boolean;
        /** CA certificate PEM file pinned for the TLS handshake; omitted uses the system trust store. */
        ca?: string;
        binary?: boolean;
        bigint?: boolean;
        /** Caps a bulk payload or streamed-string chunk total, and an aggregate or stream element count; default 64 MiB.
         *  RESP3 streamed replies (`$?` `;n` .. `;0`; `*?` `~?` `%?` `|?` .. `.`) decode as their fixed form; nesting max 31. */
        maxReplyBytes?: number;
        maxPending?: number;
        connectTimeoutMs?: number;
        commandTimeoutMs?: number;
    }
    /** A Redis client: commands return promises matched to replies by strict FIFO. Replies are UNTRUSTED: over-cap or malformed ones are refused by name and the connection torn down. */
    class Redis implements DynResource {
        constructor(opts?: RedisOptions);
        /** Sends one command (RESP3 when available). `HELLO` and `CLIENT REPLY` are refused: they change how many replies arrive, breaking positional matching. */
        command(command: string, ...args: (string | number | Uint8Array | ArrayBuffer)[]): Promise<unknown>;
        /** One round trip for many commands, one reply each; (P|S)SUBSCRIBE forms throw here (they answer per channel): issue them with command(). */
        pipeline(commands: (string | number | Uint8Array | ArrayBuffer)[][]): Promise<unknown[]>;
        /** Registers a push/message or error handler; returns the client. */
        on(event: "push" | "message" | "error", handler: (data: unknown) => void): this;
        /** The negotiated RESP protocol: 2 or 3. */
        readonly protocol: number;
        /** True once the handshake (HELLO/AUTH/SELECT) has completed. */
        readonly ready: boolean;
        /** Commands issued but not yet answered. */
        readonly pending: number;
        close(): void;
        dispose(): void;
        readonly closed: boolean;
        readonly [Symbol.dispose]: () => void;
    }
}

/** dyna:smtp — SMTP client and server (RFC 5321): STARTTLS/implicit TLS, SASL AUTH, PIPELINING, 8BITMIME, SMTPUTF8, BDAT/BINARYMIME, DSN.
 *  Parse, codec and SASL run on the pool (--io-threads 0: inline on the JS thread, top of the callback stack), TLS records on the main thread: client ~63 us/msg plain, ~62-100 us/msg STARTTLS (vs exim). */
declare module "dyna:smtp" {
    /** SmtpClient options; unknown keys throw TypeError. */
    interface SmtpOptions {
        host: string;
        /** Default 587, or 465 with tls "implicit". */
        port?: number;
        /** Default "starttls" (required: fails when not offered); "opportunistic" falls back to plaintext; "none" never upgrades. */
        tls?: "starttls" | "opportunistic" | "implicit" | "none";
        /** CA PEM file pinned for TLS; default the system trust store with hostname checking. */
        ca?: string;
        /** TLS SNI and certificate name; default host. */
        servername?: string;
        /** EHLO domain or address literal; default "localhost". */
        name?: string;
        /** AUTH identity; needs pass or token. */
        user?: string;
        pass?: string;
        /** OAuth 2.0 bearer token: OAUTHBEARER, else XOAUTH2. */
        token?: string;
        authzid?: string;
        /** Allowed SASL mechanisms; default every one except CRAM-MD5. Preference: SCRAM-SHA-256, -512, -1, PLAIN, LOGIN, CRAM-MD5. */
        mechanisms?: ("SCRAM-SHA-256" | "SCRAM-SHA-512" | "SCRAM-SHA-1" | "PLAIN" | "LOGIN" | "CRAM-MD5" | "OAUTHBEARER" | "XOAUTH2")[];
        /** Allow PLAIN, LOGIN, OAUTHBEARER, XOAUTH2 without TLS; default false (the session fails with SMTP_AUTH). */
        insecureAuth?: boolean;
        /** Connect, greeting, EHLO, TLS and AUTH deadline; default 10000. */
        connectTimeoutMs?: number;
        /** Wait for any reply; default 300000 (RFC 5321 4.5.3.2 floor for DATA). */
        commandTimeoutMs?: number;
        /** An idle connection sends QUIT after this; default 200. */
        idleTimeoutMs?: number;
        /** Concurrent connections, 1..64; default 1. Messages queue FIFO. */
        maxConnections?: number;
        /** QUIT and reconnect after this many messages; 0 = unlimited; default 100.
         *  A 421 on a connection that delivered a message requeues the in-flight one first in the queue; else it rejects. */
        maxMessagesPerConnection?: number;
        /** Highest SCRAM iteration count accepted, 4096..1000000; default 1000000. */
        scramMaxIterations?: number;
    }
    /** RFC 3461 DSN parameters; the send rejects with SMTP_LOCAL unless the server offers DSN. */
    interface SmtpDsn {
        ret?: "FULL" | "HDRS";
        /** ASCII, at most 100 octets; xtext-encoded on the wire. */
        envid?: string;
        notify?: ("NEVER" | "SUCCESS" | "FAILURE" | "DELAY")[];
        /** Send ORCPT=rfc822;<address> for ASCII recipients. */
        orcpt?: boolean;
    }
    /** One message: data is the full RFC 5322 message; bare CR/LF become CRLF and lines are dot-stuffed.
     *  Under BDAT with BODY=BINARYMIME the octets go exactly as given (RFC 3030). */
    interface SmtpMessage {
        /** Envelope sender; "" is the null reverse-path. */
        from: string;
        /** 1..1000 envelope recipients. */
        to: string | string[];
        data: string | Uint8Array | ArrayBuffer;
        /** Use BDAT when the server offers CHUNKING; BINARYMIME is used (with it) for NUL, bare CR/LF, lines over 998, 8-bit without 8BITMIME. */
        bdat?: boolean;
        dsn?: SmtpDsn;
    }
    interface SmtpRecipientError {
        recipient: string;
        code: number;
        /** RFC 3463 "class.subject.detail", or null. */
        enhanced: string | null;
        text: string;
    }
    /** Resolved when the server accepted the message for at least one recipient. */
    interface SmtpResult {
        accepted: string[];
        rejected: SmtpRecipientError[];
        code: number;
        enhanced: string | null;
        text: string;
    }
    /** Rejection value. SMTP_LOCAL refuses before sending (8-bit body without 8BITMIME, SIZE, line > 998, bad address). */
    interface SmtpError extends Error {
        code: "SMTP_REJECTED" | "SMTP_LOCAL" | "SMTP_AUTH" | "SMTP_TLS" | "SMTP_CONNECTION" | "SMTP_TIMEOUT" | "SMTP_CLOSED";
        smtpCode?: number;
        enhanced?: string | null;
        response?: string;
        rejected?: SmtpRecipientError[];
    }
    /** Counters; percentiles over the last 1024 samples in microseconds. */
    interface SmtpStats {
        messages: number;
        jobs: number;
        connections: number;
        queued: number;
        /** Pool handoff latency (submit to start, finish to callback). */
        handoffP50Us: number;
        handoffP99Us: number;
        /** Main-thread time per message in this module's callbacks (TLS record work excluded). */
        mainP50Us: number;
        mainP99Us: number;
    }
    /** SMTP client with a FIFO queue over up to maxConnections sessions. Every failure rejects with SmtpError. */
    class SmtpClient implements DynResource {
        constructor(opts: SmtpOptions);
        /** Queues one message; throws TypeError/RangeError on bad arguments, rejects with SmtpError. */
        send(msg: SmtpMessage): Promise<SmtpResult>;
        /** Sends QUIT on every session once the queue drains; later send() throws. */
        quit(): Promise<void>;
        stats(): SmtpStats;
        close(): void;
        dispose(): void;
        readonly closed: boolean;
        readonly [Symbol.dispose]: () => void;
    }
    /** SmtpServer options; unknown keys throw TypeError, out-of-range numbers RangeError. */
    interface SmtpServerOptions {
        /** Bind address; default "127.0.0.1". */
        host?: string;
        /** Default 25, or 465 with tls "implicit"; 0 = ephemeral (read .port after listen()). */
        port?: number;
        /** Domain in the greeting and EHLO reply; default "localhost". */
        name?: string;
        /** Default "starttls" when cert is given, else "none"; STARTTLS discards plaintext pipelined after the command (RFC 3207). */
        tls?: "none" | "starttls" | "implicit";
        /** PEM file paths; required unless tls is "none" (TypeError). */
        cert?: string;
        key?: string;
        /** Refuse MAIL before STARTTLS (530 5.7.0). */
        requireTls?: boolean;
        /** Refuse MAIL before AUTH (530 5.7.0); TypeError without onAuth. */
        requireAuth?: boolean;
        /** Allow AUTH without TLS; default false (538 5.7.11). */
        insecureAuth?: boolean;
        /** Offered SASL mechanisms; default all below; CRAM-MD5 is a RangeError. AUTH is offered only with onAuth. */
        mechanisms?: ("SCRAM-SHA-256" | "SCRAM-SHA-512" | "SCRAM-SHA-1" | "PLAIN" | "LOGIN" | "OAUTHBEARER" | "XOAUTH2")[];
        /** EHLO extensions not to offer, e.g. ["CHUNKING"]; dropping CHUNKING drops BINARYMIME; unknown names RangeError. */
        disable?: ("PIPELINING" | "8BITMIME" | "SMTPUTF8" | "DSN" | "CHUNKING" | "BINARYMIME" | "SIZE" | "ENHANCEDSTATUSCODES")[];
        /** SIZE limit in octets, 1..2^40; default 10485760 (552 past it). */
        maxMessageSize?: number;
        /** Per transaction, default 100 (452 past it). */
        maxRecipients?: number;
        /** Bad commands before 421, default 10. */
        maxErrors?: number;
        /** Messages before 421 4.7.0, default 100. */
        maxMessagesPerSession?: number;
        /** Failed AUTH before 421 4.7.0, default 3. */
        maxAuthFailures?: number;
        /** Concurrent sessions; one more gets 421 4.7.0 and is closed; default 100. */
        maxConnections?: number;
        /** Idle limits in ms, 1..3600000, default 300000; 421 then close. */
        commandTimeoutMs?: number;
        dataTimeoutMs?: number;
        /** onChunk size bound in octets, 1024..16 MiB; default 65536. */
        chunkSize?: number;
        onHelo?: (session: SmtpSession, helo: { name: string; ehlo: boolean }) => SmtpVerdict | Promise<SmtpVerdict>;
        /** PLAIN/LOGIN carry password, OAuth token; SCRAM returns SmtpScramCreds (false denies). */
        onAuth?: (session: SmtpSession, auth: SmtpAuthAttempt) => SmtpVerdict | SmtpScramCreds | Promise<SmtpVerdict | SmtpScramCreds>;
        onMail?: (session: SmtpSession, mail: SmtpMailFrom) => SmtpVerdict | Promise<SmtpVerdict>;
        onRcpt?: (session: SmtpSession, rcpt: SmtpRcptTo) => SmtpVerdict | Promise<SmtpVerdict>;
        /** Body bytes as they arrive (unstuffed, <= chunkSize, the buffer is handed over); the next chunk waits for a returned promise. A throw makes onData's reply 451. */
        onChunk?: (session: SmtpSession, chunk: Uint8Array) => void | Promise<void>;
        /** After the last body octet; data holds the whole body unless onChunk is set. */
        onData?: (session: SmtpSession, msg: SmtpEnvelope) => SmtpVerdict | Promise<SmtpVerdict>;
    }
    /** One connection; the same object is passed to every hook of the session. */
    interface SmtpSession {
        id: number;
        remoteAddress: string;
        remotePort: number;
        tls: boolean;
        helo: string | null;
        /** Set after a successful AUTH. */
        user: string | null;
    }
    /** undefined/true accept; false refuses (550 5.7.1; RCPT 550 5.1.1; AUTH 535 5.7.8); a throw or rejection gives 451 4.3.0 (454 4.7.0 for AUTH). */
    type SmtpVerdict = void | boolean | { code: number; message: string; enhanced?: string };
    interface SmtpAuthAttempt {
        mechanism: "PLAIN" | "LOGIN" | "XOAUTH2" | "OAUTHBEARER" | "SCRAM-SHA-1" | "SCRAM-SHA-256" | "SCRAM-SHA-512";
        username: string;
        authzid: string | null;
        password?: string;
        token?: string;
    }
    /** RFC 5802 stored credentials; storedKey and serverKey are the digest length (20, 32, 64); salt 1..256 bytes. The proof is checked on the pool. */
    interface SmtpScramCreds {
        salt: BytesInput;
        iterations: number;
        storedKey: BytesInput;
        serverKey: BytesInput;
    }
    interface SmtpMailFrom {
        address: string;
        body?: "8BITMIME" | "BINARYMIME";
        smtputf8: boolean;
        ret?: "FULL" | "HDRS";
        envid: string | null;
    }
    interface SmtpRcptTo {
        address: string;
        /** RFC 3461 NOTIFY value as sent, e.g. "SUCCESS,FAILURE". */
        notify: string | null;
        orcpt: string | null;
    }
    interface SmtpEnvelope extends Omit<SmtpMailFrom, "address"> {
        from: string;
        to: SmtpRcptTo[];
        /** Body octets (unstuffed). */
        size: number;
        data?: Uint8Array;
    }
    /** Counters; percentiles in microseconds over the last 1024 samples. */
    interface SmtpServerStats {
        connections: number;
        sessions: number;
        /** Connections refused by maxConnections. */
        refused: number;
        messages: number;
        jobs: number;
        /** Hook throws, rejections and invalid verdicts. */
        hookErrors: number;
        handoffP50Us: number;
        handoffP99Us: number;
        /** Main-thread time per message in this module's callbacks, hooks excluded. */
        mainP50Us: number;
        mainP99Us: number;
    }
    /** SMTP server; command parse, unstuffing, SASL and SCRAM run as pool jobs, hooks on the main thread.
     *  Main thread incl. TLS records and reactor, N=2048: ~33 us/msg plain, ~42 STARTTLS (1 KiB); ~51 / ~77 (64 KiB). */
    class SmtpServer implements DynResource {
        constructor(opts: SmtpServerOptions);
        /** Binds and accepts; returns the port; throws InternalError when the bind fails, TypeError when called twice. */
        listen(): number;
        readonly port: number;
        stats(): SmtpServerStats;
        /** Stops accepting; open sessions get 421 4.3.2 and are closed. */
        close(): void;
        dispose(): void;
        readonly closed: boolean;
        readonly [Symbol.dispose]: () => void;
    }
}

/** dyna:dkim — DKIM signing and verification (RFC 6376, RFC 8463 Ed25519, RFC 8301 no rsa-sha1).
 *  The message is copied at call; parse, canonicalization, hashing and crypto run on the shared pool (inline under --io-threads 0); option errors throw TypeError/RangeError synchronously. */
declare module "dyna:dkim" {
    /** "simple" or "relaxed" for header/body; one word means header with body "simple". */
    type DkimCanonicalization = "relaxed/relaxed" | "relaxed/simple" | "simple/relaxed" | "simple/simple" | "relaxed" | "simple";
    interface DkimSignOptions {
        /** d=; dot-separated labels of letters, digits, "-", "_" or UTF-8 (RFC 8616 U-labels), no empty label, at most 256 bytes. */
        domain: string;
        /** s=; printable ASCII without ";" or space, at most 256 bytes. */
        selector: string;
        /** PEM private key (PKCS#8 or PKCS#1; RSA >= 1024 bits or Ed25519), or a 32-byte Ed25519 seed. */
        privateKey: string | ByteView;
        /** Must match the key type when given (else the promise rejects); default from the key. */
        algorithm?: "rsa-sha256" | "ed25519-sha256";
        /** Default "relaxed/relaxed". */
        canonicalization?: DkimCanonicalization;
        /** Header names to sign (1..128, must include From); every present instance is signed bottom-up.
         *  Default: from to cc subject date message-id reply-to in-reply-to references mime-version content-type content-transfer-encoding sender. */
        headers?: string[];
        /** Add one extra h= entry per listed name so a later added instance breaks the signature; default false. */
        oversign?: boolean;
        /** i=; must be within domain at verify time. */
        identity?: string;
        /** Emit l= with the canonical body length; default false. */
        bodyLength?: boolean;
        /** t= in Unix seconds; default now; null omits t=. */
        timestamp?: number | null;
        /** x= = t (or now) + expiresIn seconds, >= 1. */
        expiresIn?: number;
    }
    /** Lookup of "<selector>._domainkey.<domain>": string (one record), string[] (its chunks), string[][] (resolveTxt shape, first record), null = no record.
     *  A rejection whose message has "rcode 3", NXDOMAIN or ENOTFOUND is permerror; any other is temperror. */
    type DkimLookup = ((name: string) => string | string[] | string[][] | null | Promise<string | string[] | string[][] | null>) | { resolveTxt(name: string): Promise<string[][]> };
    interface DkimVerifyOptions {
        /** Required, no default resolver: a function or any object with resolveTxt() (e.g. a dyna:dns DNSResolver), else TypeError (thrown).
         *  Lookups for all signatures are started before any is awaited. */
        lookup: DkimLookup;
        /** Unix seconds for the x= check; default now. */
        now?: number;
        /** Signatures evaluated top-down (1..64, default 5); later ones report neutral; at most 64 are reported. */
        maxSignatures?: number;
        /** RSA keys below this are permerror (512..16384, default 1024 per RFC 8301). */
        minRsaBits?: number;
    }
    /** One DKIM-Signature field, in header order. pass/fail/neutral/temperror/permerror (RFC 8601 result names). */
    interface DkimResult {
        status: "pass" | "fail" | "neutral" | "temperror" | "permerror";
        domain?: string;
        selector?: string;
        algorithm?: "rsa-sha256" | "ed25519-sha256";
        identity?: string;
        signedHeaders?: string[];
        bodyLength?: number;
        timestamp?: number;
        expires?: number;
        /** Key record t=y. */
        testing?: boolean;
        reason?: string;
    }
    /** Resolves the DKIM-Signature field (folded at 76 columns, CRLF-terminated) to prepend to the message.
     *  Bare CR or LF counts as CRLF; at most 1000 header fields; rejects on a malformed header, a missing From or a bad key. */
    function sign(message: BytesInput, opts: DkimSignOptions): Promise<string>;
    /** Resolves one result per signature ([] when unsigned); never rejects for message content: a malformed header or over 1000 fields is one permerror.
     *  Key records over 64 KiB and tag-lists over 64 tags are permerror; p= is SubjectPublicKeyInfo or bare RSAPublicKey (RSA), 32 raw bytes (Ed25519). */
    function verify(message: BytesInput, opts: DkimVerifyOptions): Promise<DkimResult[]>;
}

/** dyna:sqlite — an embedded SQLite handle over the linked libsqlite3 (the only module that pulls it in).
 *  Split out of dyna:net as a clean break: dyna:net no longer exports SQLite. Absent from builds without SQLite. */
declare module "dyna:sqlite" {
    /** SQLite options. */
    interface SQLiteOptions {
        readonly?: boolean;
        bigint?: boolean;
        /** Default true: a writable file database switches to WAL mode with synchronous=NORMAL when the file allows it (a lock held by another connection, or a filesystem without shared memory, keeps the rollback journal) (readers do not block the writer; a power loss can drop the last commits but never corrupts). false keeps SQLite's rollback journal and synchronous=FULL. In-memory and readonly opens are unchanged. A WAL database is the file plus its -wal file, so back it up with `VACUUM INTO`, not a file copy. */
        wal?: boolean;
    }
    /** A SQLite handle (defensive mode, trusted_schema off); values are bound, never interpolated; exec() returns rows changed; --timeout-ms interrupts a running statement. */
    class SQLite implements DynResource {
        constructor(path: string | import("dyna:file").Path, opts?: SQLiteOptions);
        /** Runs a statement and returns one object per row; duplicate column names collapse to the last one, so alias them in the SELECT. */
        query(sql: string, params?: unknown[]): Record<string, unknown>[];
        /** Runs a statement without result rows; returns the number of rows changed. */
        exec(sql: string, params?: unknown[]): number;
        /** Runs one statement once per row (each row a dense array of its parameters) inside a savepoint and returns the rows changed in total. All rows or none: a failing row (constraint, bad value, wrong parameter count) rolls the whole batch back and throws naming the row; an enclosing BEGIN stays open with its earlier work. Transaction control (BEGIN, COMMIT, SAVEPOINT, ...) and multi-statement text are refused. */
        execMany(sql: string, rows: unknown[][]): number;
        /** The rowid of the most recent successful INSERT. */
        readonly lastInsertRowId: number;
        /** The linked library's version, read at runtime. */
        readonly version: string;
        close(): void;
        dispose(): void;
        readonly closed: boolean;
        readonly [Symbol.dispose]: () => void;
    }
}


/** src/pool.js — PgPool and RedisPool: pure-JS pooling over the dyna:postgres and dyna:redis clients, shipped as a source file, NOT a builtin.
 *  Import by relative path (`import { PgPool } from "./src/pool.js"`); the wildcard module name below matches every such spelling. */
declare module "*/src/pool.js" {
    /** Connection-pool options shared by PgPool and RedisPool. */
    interface PoolOptions {
        /** Max simultaneous connections; the pool refuses to grow past this. */
        size?: number;
        /** Idle connections are evicted after this many milliseconds (0 = never). */
        idleMs?: number;
        /** How long acquire() waits for a free connection before timing out. */
        acquireTimeoutMs?: number;
    }
    interface PgPoolOptions extends PoolOptions {
        host?: string;
        port?: number;
        user?: string;
        password?: string;
        database?: string;
        path?: string;
        tls?: boolean;
        /** a CA bundle path/PEM pinned by every pooled connection (same contract as PostgreSQL's `ca`). */
        ca?: string;
        stmtCacheMax?: number;
        maxPending?: number;
    }
    /** A bounded pool of PostgreSQL clients (size 4, idleMs 30000, acquireTimeoutMs 5000); query/pipeline auto acquire-release, with(fn) pins one client; release() ignores a connection already free. */
    class PgPool {
        constructor(opts?: PgPoolOptions);
        readonly stats: { total: number; free: number; used: number; waiting: number; closed: boolean };
        query(sql: string, params?: unknown[], opts?: { maxRows?: number }): Promise<unknown>;
        /** One round trip; after a failed member the server skips the rest, and a later re-run re-prepares. */
        pipeline(stmts: [string, ...unknown[]][]): Promise<unknown[]>;
        /** Pins one client from the pool for a transaction or multi-step flow. */
        with<T>(fn: (client: import("dyna:postgres").PostgreSQL) => Promise<T>): Promise<T>;
        close(): Promise<void>;
    }
    interface RedisPoolOptions {
        host?: string;
        port?: number;
        path?: string;
        user?: string;
        password?: string;
        protocol?: 2 | 3;
        size?: number;
        acquireTimeoutMs?: number;
    }
    /** A bounded pool of Redis clients (size 4, acquireTimeoutMs 5000); the ACL user is spelled `user`; a failed command retires its connection. */
    class RedisPool {
        constructor(opts?: RedisPoolOptions);
        readonly stats: { total: number; free: number; used: number; waiting: number; closed: boolean };
        command(command: string, ...args: (string | number | Uint8Array | ArrayBuffer)[]): Promise<unknown>;
        pipeline(commands: (string | number | Uint8Array | ArrayBuffer)[][]): Promise<unknown[]>;
        close(): Promise<void>;
    }
}


/** dyna:json — JSON editing: Pointer (RFC 6901) addressing, Patch (RFC 6902) with copy-on-write, buffered NDJSON. */
declare module "dyna:json" {
    // Neighbours: stable stringify (RFC 8785), JSON5 and JSONPath live in dyna:encoding; streaming NDJSON is dyna:stream's ndjson().

    /** RFC 6901 JSON Pointer. */
    namespace Pointer {
        /** Walks pointer; missing members and out-of-range indices throw. */
        function get<T = unknown>(doc: unknown, pointer: string): T;
        /** Same walk; a missing target returns false instead of throwing. */
        function has(doc: unknown, pointer: string): boolean;
        /** Mutates doc in place (RFC 6902 add semantics) and returns it. */
        function set<T>(doc: T, pointer: string, value: unknown): T;
        /** Mutates doc in place and returns it. */
        function remove<T>(doc: T, pointer: string): T;
        /** ~ -> ~0 and / -> ~1. */
        function escape(token: string): string;
        /** Reverses escape; throws on a `~` not followed by 0 or 1. */
        function unescape(token: string): string;
    }

    /** RFC 6902 JSON Patch. */
    const Patch: {
        /** Runs the six RFC 6902 ops on a PRIVATE deep copy of plain data; non-plain values (Date/RegExp/Map/TypedArray) are shared by reference and the input is never written. */
        apply(doc: unknown, ops: { op: string; path: string; value?: unknown; from?: string }[]): unknown;
    };

    /** NDJSON, buffer-at-once (the streaming form is dyna:stream's ndjson); lines split on LF with one trailing CR stripped; blank lines are skipped. */
    namespace Ndjson {
        /** Parses every non-blank line; a malformed one throws SyntaxError naming its 1-based line number in the text. */
        function parse(text: string): unknown[];
        /** One compact JSON document per line with a trailing newline; an item with no JSON form throws TypeError naming its index. */
        function stringify(items: unknown[]): string;
    }
}

/** dyna:log — leveled structured logging: JSON lines or text, to stderr, a rolling file or a custom sink; control characters are escaped so a value cannot forge a line. */
declare module "dyna:log" {
    /** Log levels, least to most severe; "silent" disables output. */
    type LogLevel = "trace" | "debug" | "info" | "warn" | "error" | "fatal" | "silent";
    /** A leveled logger; child() derives one that adds fields to every line. */
    class Logger {
        /** Creates a logger; the ctor bag, its rollover sub-bag and child()'s options are all strict. */
        constructor(opts?: {
            level?: LogLevel;
            name?: string;
            timestamp?: "epoch" | "iso" | false;
            base?: Record<string, unknown>;
            /** Output path (string or Path), or a sink `(line) => void` receiving each formatted line unbuffered; rollover needs a path. Default: stderr. */
            dest?: string | import("dyna:file").Path | ((line: string) => void);
            /** Emit sampling: every Nth entry passing the level gate is written (deterministic counter per logger). Default 1. */
            sample?: number;
            /** Create dest's parent directories. */
            mkdir?: boolean;
            /** Batch file output: true = 64 KiB, or a byte size 1..1048576; flushed at normal exit (a crash loses the tail). */
            buffer?: boolean | number;
            /** Add "pid" to every line. */
            pid?: boolean;
            /** Add "hostname" to every line. */
            hostname?: boolean;
            /** "json" (default) or "text" (`LEVEL name: msg k=v`). */
            format?: "json" | "text";
            /** File rollover (needs dest). Lazy: checked before each emit. Lines go to numbered files beside dest (`app.log` -> `app.1.log`, ...); `dest` itself exists only with `symlink`. */
            rollover?: {
                /** Rotate at this size: bytes, or "500k"/"10m"/"2g". */
                size?: number | string;
                /** Rotate on this interval: "daily", "hourly", or milliseconds. */
                frequency?: "daily" | "hourly" | number;
                /** Rotated files to keep (plus the active one). */
                count?: number;
                /** Keep `dest` as a symlink to the active file; a pre-existing symlink is replaced, never written through. readFile follows it, so dest reads the active file. */
                symlink?: boolean;
            };
        });
        /** Each emits one line at its level. Shapes: (msg), (fields, msg), (err, msg), (err, fields, msg); a field colliding with a frame key (time/level/name/pid/hostname/msg/err) is dropped. */
        trace(msg: unknown, ...fields: unknown[]): void;
        debug(msg: unknown, ...fields: unknown[]): void;
        info(msg: unknown, ...fields: unknown[]): void;
        warn(msg: unknown, ...fields: unknown[]): void;
        error(msg: unknown, ...fields: unknown[]): void;
        fatal(msg: unknown, ...fields: unknown[]): void;
        /** A new Logger with fields appended to the base prefix; shares the parent's destination. The optional second argument is a strict options bag: `{ level }` only (fields is NOT a bag). */
        child(fields: Record<string, unknown>, opts?: { level?: LogLevel }): Logger;
        /** Whether the level passes the current threshold. */
        enabled(level: LogLevel): boolean;
        /** Land any buffered file output now. A no-op on stderr. */
        flush(): void;
        /** Get/set the level. */
        level: LogLevel;
    }

    /** Returns a printer active only when $DEBUG matches the namespace: comma-separated patterns, `-` negates, one `*` wildcard, last match wins. */
    function Debug(namespace: string): (...args: unknown[]) => void;
}

/** dyna:matcher — text search and similarity: compiled single- and multi-pattern (Aho-Corasick) matchers, edit distances, tries, Bloom filters, diffs. */
declare module "dyna:matcher" {
    /** A compiled single-pattern matcher. */
    class Matcher {
        /** `algo` is "kmp" (default) or "bmh" (alias "boyer-moore"); a pure-ASCII pattern searches the subject's bytes directly. */
        constructor(pattern: string, opts?: { algo?: "kmp" | "bmh" | "boyer-moore" });
        /** Code-unit offset of the first match from `fromIndex` (String.prototype.indexOf conventions), or -1. */
        firstIn(text: string, fromIndex?: number): number;
        /** True when a pattern occurs in `text`. */
        test(text: string): boolean;
        /** Number of matches in `text`. */
        countIn(text: string): number;
        /** Every match offset. */
        allIn(text: string): number[];
        /** Non-overlapping left-to-right replacement. */
        replaceAllIn(text: string, repl: string): string;
        readonly length: number;
        readonly algo: string;
    }

    /** A byte-trie Aho-Corasick multi-pattern matcher. Identical patterns are deduplicated: N identical hits at one position emit one, at the first index. */
    class MultiMatcher {
        constructor(patterns: string[]);
        /** The pattern index and offset of the earliest hit, or null. */
        firstIn(text: string): { index: number; at: number } | null;
        /** True when a pattern occurs in `text`. */
        test(text: string): boolean;
        /** Every emitted hit (overlapping matches each count). */
        countIn(text: string): number;
        /** Every hit as { index: pattern number, at: position }, ordered by end position (longest first at one end). */
        allIn(text: string): { index: number; at: number }[];
        /** Replaces non-overlapping hits left to right in one pass; `repl` is a string or repl(match, patternIndex). */
        replaceAllIn(text: string, repl: string | ((match: string, index: number) => string)): string;
        /** Writes every hit of the patterns in `buf` (UTF-8 bytes) into caller-owned Int32Arrays and returns the TOTAL hit count; no per-hit JS allocation, synchronous.
         *  Offsets are byte offsets into `buf`, end exclusive (outStarts[i] <= outEnds[i]); outIds[i] is the pattern index. Identical patterns report only the first index.
         *  Default: every overlapping hit in allIn order (by end, longest first at one end). `longestOnly`: leftmost-longest non-overlapping, ordered by start: at the
         *  leftmost start take the longest pattern, then resume at its end; a tie at one start and length is one byte string, so the lowest pattern index wins.
         *  Capacity: when the total exceeds the arrays' common length, the first `length` hits are written, the rest of the scan only counts, and the total is still
         *  returned, so the caller can grow the arrays to the returned count and call again; slots past the written prefix are left untouched.
         *  TypeError: buf not a Uint8Array, an out array not an Int32Array, out lengths differ, a detached or out-of-bounds view (a resizable buffer is read at its
         *  current length), an immutable out buffer, any two of the four views sharing memory, opts not a plain object, an unknown opts key, a non-boolean longestOnly.
         *  RangeError: buf longer than 2^31 - 1 bytes; past the scan step budget the same RangeError as allIn. Options are read before the views. */
        matchSpansInto(buf: Uint8Array, outStarts: Int32Array, outEnds: Int32Array, outIds: Int32Array, opts?: { longestOnly?: boolean }): number;
        readonly size: number;
        readonly states: number;
    }

    /** Exact edit distance in code points, Myers bit-parallel below 64. */
    function Levenshtein(a: string, b: string, opts?: { max?: number }): number;
    /** Bigram multiset similarity in [0, 1]. */
    function DiceCoefficient(a: string, b: string): number;
    /** Jaro similarity with Winkler's prefix boost (prefix capped at 4, scale 0.1) in [0, 1]; code-point operands. */
    function JaroWinkler(a: string, b: string): number;
    /** Restricted Damerau-Levenshtein (optimal string alignment) in code points; exact while <= max, max + 1 beyond. */
    function DamerauLevenshtein(a: string, b: string, opts?: { max?: number }): number;
    /** Local alignment (linear gap) of two strings by code point, or two 4-byte typed arrays by raw 32-bit token; ties prefer diagonal, then a-gap (I), then b-gap (D).
     *  band/diagonal keep only cells with j - i in [diagonal - band, diagonal + band]; ranges are half-open; cigar uses M/I/D (I consumes a, D consumes b). */
    function SmithWaterman(a: string | Int32Array | Uint32Array, b: string | Int32Array | Uint32Array, opts?: { match?: number; mismatch?: number; gap?: number; band?: number; diagonal?: number; traceback?: boolean }):
        { score: number; aStart: number; aEnd: number; bStart: number; bEnd: number; matches?: number; cigar?: string };
    /** A diff hunk: -1 deleted, 1 inserted, 0 common. */
    interface DiffHunk {
        op: -1 | 0 | 1;
        text: string;
    }
    /** Options of DiffChars, DiffWords and DiffLines: only the dyna meta bag. `dyna.maxSteps` raises the step budget's ceiling (an integer in [2^31, 2^53-1]; a smaller value is a RangeError because the meta config can only raise it) or removes the budget with Infinity: plain Myers then runs to the end with no fallback, O((N+M)D) time. Unknown keys are a TypeError. */
    interface DiffOptions {
        dyna?: { maxSteps?: number };
    }
    /** Myers diff by character in linear space (middle snake, explicit work stack): memory is O(N+M) tokens. The step budget is 2(N+M)^2 diagonal steps clamped to [2^28, 2^31] (opts.dyna.maxSteps raises the ceiling or removes it). Past the budget, a pair whose stripped sides multiply to at most 2^24 cells gets the longest-common-subsequence script instead (computed with row checkpoints, still O(N+M) memory; the script prefers a match, then an insertion); a larger pair throws RangeError("inputs too dissimilar"). */
    function DiffChars(a: string, b: string, opts?: DiffOptions): DiffHunk[];
    /** Myers diff tokenised by word; same linear-space Myers, budget, fallback and opts.dyna.maxSteps. */
    function DiffWords(a: string, b: string, opts?: DiffOptions): DiffHunk[];
    /** Myers diff tokenised by line; same linear-space Myers, budget, fallback and opts.dyna.maxSteps. */
    function DiffLines(a: string, b: string, opts?: DiffOptions): DiffHunk[];
    /** Unified diff text of two texts by line (DiffLines' Myers script and budget), in `diff -u` format: "--- aName\n+++ bName\n" (defaults "a", "b"; no timestamps; a name may not hold a line break or NUL), then hunks "@@ -l,n +l,n @@" (",1" omitted; an empty range names the line before it, so an empty side is -0,0) with `context` unchanged lines around each change (default 3, integer 0..2^30; changes at most 2*context apart share a hunk); deletions precede insertions within a change. A last line without "\n" is followed by "\ No newline at end of file". Identical texts give "". `dyna.maxSteps` is DiffLines' meta option. Options are strict (TypeError naming the valid set). */
    function UnifiedDiff(a: string, b: string, opts?: { context?: number; aName?: string; bName?: string; dyna?: { maxSteps?: number } }): string;
    /** Applies one file's unified diff (UnifiedDiff's, `diff -u`'s or git's) to `text`; ApplyPatch(a, UnifiedDiff(a, b)) === b. Lines before the first "@@" and after a hunk's counted lines (diff/index/---/+++ headers, mail signatures) are skipped; an empty patch line inside a hunk is an empty context line; "\ No newline at end of file" makes the line above it end without "\n". Hunks apply strictly at the old-side line numbers they state (no offset search, no fuzz), in order: a context or removed line that differs from the text (its final newline included) is an Error naming the hunk and line, as is a hunk before the previous one's end or past the text. A malformed header, a truncated hunk, an unexpected line tag or a second file ("--- " + "+++ " after a hunk) is a SyntaxError naming the patch line. An empty patch returns `text`. */
    function ApplyPatch(text: string, patch: string): string;
    /** The Snowball English (Porter2) stemmer, https://snowballstem.org/algorithms/english/stemmer.html, as Snowball 3 generates it: every one of
     *  the 42,649 pairs of the snowball-data english voc.txt -> output.txt reference matches (tests/corpus/snowball/english). It covers the 3.x
     *  rules: R1 prefixes gener commun arsen past univers later emerg organ inter, -ogist -> og, Cying -> Cie, no undoubling after a single letter
     *  (add, ebb, err, off), "evening" invariant. The word is taken as given: pass it lowercase; letters other than a-z and the apostrophe are
     *  non-vowels, and lengths and regions are counted in code points ("éying" -> "éie"). A non-string is a TypeError. */
    function stem(word: string): string;
}

/** dyna:mathx — the math toolbox: constants, special functions, combinatorics, number theory, bit ops, linear algebra, and `stats` reductions.
 *  Statistics come in three layers: Array.prototype conveniences, mathx.stats (compensated sums, Float64Array fast path), DataFrame verbs (columnar, masked, histograms). */

declare module "dyna:mathx" {
    /** Mathematical constants, written with enough digits for one correctly-rounded conversion. */
    const E: number;
    const Pi: number;
    const Phi: number;
    const Sqrt2: number;
    const SqrtE: number;
    const SqrtPi: number;
    const Ln2: number;
    const Ln10: number;
    const Log2E: number;
    const Log10E: number;
    const MaxInt32: number;
    const MinInt32: number;
    const MaxSafeInteger: number;
    const MaxInt64: bigint;

    /** Smallest positive normal double (DBL_MIN). */
    function realmin(): number;
    /** Largest finite double (DBL_MAX). */
    function realmax(): number;
    /** 2^53, the largest integer every double below it represents exactly. */
    function flintmax(): number;
    /** The gap to the next double away from zero (eps() is eps(1)); eps(realmax) is 2^971. */
    function eps(x?: number): number;

    /** C99 round (ties away from zero). */
    function round(x: number): number;
    /** Round half to even. */
    function roundToEven(x: number): number;
    /** Truncate toward zero. */
    function fix(x: number): number;
    /** 1, -1, or x itself (-0 and NaN pass through). */
    function sign(x: number): number;
    /** The sign bit directly. */
    function signbit(x: number): boolean;
    /** C trunc. */
    function trunc(x: number): number;
    /** Splits into [intPart, fracPart]. */
    function modf(x: number): [number, number];

    /** MATLAB floored modulo: mod(-7, 3) is 2; mod(a, 0) is a. */
    function mod(a: number, b: number): number;
    /** Truncated fmod. */
    function rem(a: number, b: number): number;
    /** The C99 truncated remainder. */
    function fmod(a: number, b: number): number;
    /** C99 round-to-nearest remainder. */
    function remainder(a: number, b: number): number;
    /** Integer division with an explicit rounding mode: "fix"|"floor"|"ceil"|"round". */
    function idivide(a: number, b: number, mode?: "fix" | "floor" | "ceil" | "round"): number;
    /** The real n-th root; defined for negative x with odd integer n. */
    function nthroot(x: number, n: number): number;

    /** Gamma function (tgamma). */
    function gamma(x: number): number;
    function cbrt(x: number): number;
    function hypot(a: number, b: number): number;
    function copysign(a: number, b: number): number;
    /** The next representable double after `a` in the direction of `b`. */
    function nextafter(a: number, b: number): number;
    function expm1(x: number): number;
    function log1p(x: number): number;
    function log2(x: number): number;
    /** The unbiased floating-point exponent (logb). */
    function logb(x: number): number;
    /** 2^x (exp2), the inverse of log2. */
    function pow2(x: number): number;
    function deg2rad(x: number): number;
    function rad2deg(x: number): number;
    /** The smallest p with 2^p >= |x|; nextpow2(0) is 0. */
    function nextpow2(x: number): number;
    /** x * 2**n. */
    function scalbn(x: number, n: number): number;
    /** Identical to scalbn (MATLAB spelling). */
    function ldexp(frac: number, exp: number): number;
    /** Splits into x = frac * 2**exp with |frac| in [0.5, 1). */
    function frexp(x: number): [number, number];
    /** ±Inf and NaN give 2^31-1, 0 gives -(2^31), else the unbiased exponent. */
    function ilogb(x: number): number;
    /** Tests infinity, optionally restricted to one sign. */
    function isInf(x: number, sign?: number): boolean;
    function isNaN(x: number): boolean;

    function erf(x: number): number;
    function erfc(x: number): number;
    /** Inverts erf; erfinv(±1) is ±Inf. */
    function erfinv(y: number): number;
    /** erfinv(1 - y), domain [0, 2]. */
    function erfcinv(y: number): number;
    /** exp(x^2)*erfc(x), finite where erfc underflows. */
    function erfcx(x: number): number;

    /** [log|Gamma(x)|, sign of Gamma(x)] via the reentrant lgamma_r. */
    function lgamma(x: number): [number, number];
    /** log|Gamma| without the sign. */
    function gammaln(x: number): number;
    /** Computed through lgamma, so it does not overflow for moderate arguments the way `tgamma(a)*tgamma(b)/tgamma(a+b)` does. */
    function beta(a: number, b: number): number;
    /** Natural log of the beta function. */
    function betaln(a: number, b: number): number;
    /** Digamma; psi(0) is ±Inf, negative integers are NaN. */
    function psi(x: number): number;
    /** n-th derivative, order in [0, 64]; negative non-integer x by reflection, poles are NaN. */
    function polygamma(n: number, x: number): number;
    /** Regularised incomplete gamma (x first); "upper" selects the complement. */
    function gammainc(x: number, a: number, tail?: "upper"): number;
    /** Inverts P(a, x) = p. */
    function gammaincinv(p: number, a: number): number;
    /** Regularised incomplete beta. */
    function betainc(x: number, a: number, b: number): number;
    /** Inverse of the regularized incomplete beta function. */
    function betaincinv(p: number, a: number, b: number): number;
    /** E1(x), defined for x > 0. */
    function expint(x: number): number;

    /** Integer order only. |order| above 16777216 that would not underflow at x is a RangeError. */
    function besselj(n: number, x: number): number;
    /** Bessel function of the second kind, Y_n(x). */
    function bessely(n: number, x: number): number;
    /** Real order. Measured within 4 ULP of mpmath for -1 < nu <= 600 (plus nu up to 2000 grid rows), 1e-3 <= x <= 700 (tests/mathx/test_mathx_ref.js grid plus a 1500-point random sweep): below x = 20 (or while the asymptotic first term is large) the power series sums in doubles that carry each term's rounding error (fma error-free products, compensated sum; dropping the product error alone costs 5 ULP at nu = 100, x = 700) and (x/2)^nu / Gamma(nu + 1) is built from exact products (four interleaved compensated chains for Gamma, so nu = 100 costs ~0.3 us), never exp(nu ln(x/2) - lgamma(nu + 1)), whose rounded exponent alone cost |exponent| ULPs; the unscaled value is computed directly, so it no longer underflows to 0 where I_nu(x) e^-x does (besseli(500, 100) = 3.59e-283). Orders 150 <= nu <= 1e6 (and 40 <= nu < 150 when x > 20, where the series is slower) use Debye's uniform expansion (DLMF 10.41.3, U_1..U_10; truncation below 1e-16 relative from nu = 40): the exponent sqrt(nu^2 + x^2) + nu ln(x / (nu + sqrt(nu^2 + x^2))) is formed in double-double (atanh-series log, k ln2 reduction, nu^2 / (sqrt(nu^2 + x^2) + x) for x > nu so the scaled form does not cancel), so the cost is a flat ~0.25-0.3 us at any order and the 4 ULP bound holds to nu = 1e6 (tests/mathx/test_mathx_debye.c: worst 1.8 ULP against exact finite-sum oracles; above order 2000 the old logarithmic prefactor was 3749 ULP off at nu = 2651). Orders above 1e6 or below -1 keep the logarithmic prefactor. */
    function besseli(nu: number, x: number): number;
    /** Real order. Measured within 4 ULP (worst 3) of mpmath at 120 digits (mpmath's own besselk is wrong at large order below ~80 digits) on the test grid and two 2620-point sweeps (near-integer orders, orders to 600, x from 1e-300 and 1e-10 to 1000). nu = n + mu with |mu| <= 1/2: K_mu, K_mu+1 come from Temme's series for x <= 1 (1/Gamma(1 +- mu) from its Taylor series, no lgamma differences, so near-integer orders do not cancel; beyond x = 1 the series loses a ULP) or else from the trapezoid rule on K = int e^(-x cosh t) cosh(mu t) dt (power-of-two step <= 1/(2 sqrt x); for this analytic, doubly-exponentially decaying integrand the error falls exponentially with 1/step, below 1e-20 at that step), then forward recurrence to n in double-double with exponent tracking, so the unscaled value neither overflows early nor loses the e^-x factor's bits. Orders nu >= 40 skip the O(nu) recurrence and use the same Debye uniform expansion as besseli (signs (-1)^k U_k), a flat ~0.25-0.3 us at any order. Orders above 1e6 (and non-finite) answer +Infinity -- the limit of K_nu(x) as nu grows, for any finite x > 0; kept from the recurrence era, although the Debye branch itself has no order-dependent cost. */
    function besselk(nu: number, x: number): number;
    /** I_nu(x) e^-x; same method and 4 ULP bound as besseli. */
    function besseliScaled(nu: number, x: number): number;
    /** K_nu(x) e^x; same method and 4 ULP bound as besselk; the 1e6 order clamp (+Infinity above it) applies here too. */
    function besselkScaled(nu: number, x: number): number;
    /** Hankel function J_n ± i Y_n as [re, im]; kind 1|2; the order must be an int32. */
    function besselh(n: number, x: number, kind: 1 | 2): [number, number];

    /** Complete elliptic integrals [K, E] from one AGM iteration. */
    function ellipke(m: number): [number, number];
    /** Jacobi elliptic functions {sn, cn, dn}. */
    function ellipj(u: number, m: number): { sn: number; cn: number; dn: number };

    /** Associated Legendre functions P_n^m for the whole column m = 0..n. */
    function legendre(n: number, x: number): number[];
    /** The single value P_n^m(x); degree capped at 150. */
    function legendreP(n: number, m: number, x: number): number;
    /** All four Airy values from one evaluation, each within 4 ULP of mpmath on [-30, 30] (of the envelope |x|^(-+1/4)/sqrt(pi) where the functions oscillate, x < -1): the Maclaurin series on [-12, 8] in doubles carrying each term's rounding error (fma error-free products, compensated sums: it cancels by up to 6 digits near -7 and 13 near 8), the asymptotic forms with a double-double phase zeta - pi/4 below -12 and above 10, K_{1/3}, K_{4/3} from one cancellation-free quadrature (K_{2/3} = K_{4/3} - 2/(3 zeta) K_{1/3}) for Ai and Ai' on (8, 10). */
    function airy(x: number): { ai: number; aip: number; bi: number; bip: number };

    /** Deterministic Miller-Rabin with a 12-witness set; proven for every uint64. */
    function isPrime(n: number): boolean;
    /** Ascending prime factors with multiplicity; factor(1) is []. */
    function factor(n: number): number[];
    /** Every prime <= n by sieve, up to 5e7. */
    function primes(n: number): number[];

    /** Both arguments within the int64 range; wider BigInts are refused. */
    function gcd(a: number | bigint, b: number | bigint): bigint;
    /** Both arguments within the int64 range; wider BigInts are refused. */
    function lcm(a: number | bigint, b: number | bigint): bigint;
    /** n! exactly, capped at 10000. */
    function factorial(n: number): bigint;
    /** The magnitude as an unsigned BigInt; magnitudes of 2^64 or more are refused, not truncated. */
    function abs(n: bigint): bigint;
    /** Minimum bits to represent the magnitude (bitLen(0n) is 0); magnitudes of 2^64 or more are refused. */
    function bitLen(n: bigint): number;
    /** Set bits in the magnitude; magnitudes of 2^64 or more are refused. */
    function popcount(n: bigint): number;

    /** Binomial coefficient built multiplicatively in min(k, n-k) steps; huge orders return Infinity promptly, never hang. */
    function nchoosek(n: number, k: number): number;
    /** Every permutation, reverse lexicographic, at most 8 elements. */
    function perms(v: number[]): number[][];
    /** Rational approximation by continued fractions within relative tolerance. */
    function rat(x: number, tol?: number): [number, number];

    /** n points INCLUSIVE of both ends (the last is exactly b); Number.range is the exclusive-end stepper. */
    function linspace(a: number, b: number, n?: number): number[];
    /** 10^t over the linspace grid. */
    function logspace(a: number, b: number, n?: number): number[];
    /** Array-like inputs are capped at 16777216 elements (RangeError beyond). */
    function cumsum(v: number[]): number[];
    /** The running product. */
    function cumprod(v: number[]): number[];
    /** Adjacent differences, one element shorter; array-like inputs capped at 16777216 elements (RangeError beyond). */
    function diff(v: number[]): number[];
    /** Solves A x = b for symmetric positive-definite A (row-major n x n Float64Array) by Cholesky; a non-positive pivot or asymmetric A is a RangeError. */
    function solveSPD(A: Float64Array, b: Float64Array): Float64Array;

    /** Width-parameterised fixed-width bit primitives. */
    namespace bits {
        const uintSize: number;
        function leadingZeros8(x: number): number;
        function leadingZeros16(x: number): number;
        function leadingZeros32(x: number): number;
        function leadingZeros64(x: bigint): number;
        function trailingZeros8(x: number): number;
        function trailingZeros16(x: number): number;
        function trailingZeros32(x: number): number;
        function trailingZeros64(x: bigint): number;
        function onesCount8(x: number): number;
        function onesCount16(x: number): number;
        function onesCount32(x: number): number;
        function onesCount64(x: bigint): number;
        function len8(x: number): number;
        function len16(x: number): number;
        function len32(x: number): number;
        function len64(x: bigint): number;
        function reverse8(x: number): number;
        function reverse16(x: number): number;
        function reverse32(x: number): number;
        function reverse64(x: bigint): bigint;
        function reverseBytes16(x: number): number;
        function reverseBytes32(x: number): number;
        function reverseBytes64(x: bigint): bigint;
        function rotateLeft8(x: number, k: number): number;
        function rotateLeft16(x: number, k: number): number;
        function rotateLeft32(x: number, k: number): number;
        function rotateLeft64(x: bigint, k: number): bigint;
        /** Rotate right; k reduces modulo the width, so a negative k rotates left. */
        function rotateRight8(x: number, k: number): number;
        function rotateRight16(x: number, k: number): number;
        function rotateRight32(x: number, k: number): number;
        function rotateRight64(x: bigint, k: number): bigint;
        /** Widening add with carry in and out. */
        function add32(a: number, b: number, carry: number): [number, number];
        function add64(a: bigint, b: bigint, carry: bigint): [bigint, bigint];
        function sub32(a: number, b: number, borrow: number): [number, number];
        function sub64(a: bigint, b: bigint, borrow: bigint): [bigint, bigint];
        /** Full-width product, high word first. */
        function mul32(a: number, b: number): [number, number];
        function mul64(a: bigint, b: bigint): [bigint, bigint];
        /** Divides the double-width hi:lo by y; throws on y==0 or y<=hi. */
        function div32(hi: number, lo: number, y: number): [number, number];
        function div64(hi: bigint, lo: bigint, y: bigint): [bigint, bigint];
        function rem32(hi: number, lo: number, y: number): number;
        function rem64(hi: bigint, lo: bigint, y: bigint): bigint;
    }

    /** Reductions over a Float64Array (fast path) or any array-like (capped at 16777216 elements). sum is Neumaier-compensated; median/quantile use R-7.
     *  Unlike DataFrame: empty input and zero variance THROW RangeError and a NaN element answers NaN; min/max follow Math.min/Math.max. */
    namespace stats {
        /** Compensated sum; empty array sums to +0. */
        function sum(a: ArrayLike<number>): number;
        /** sum / n; the empty array is 0/0 = NaN. */
        function mean(a: ArrayLike<number>): number;
        /** Two-pass variance, sample (n-1) by default; opts.pop divides by n. */
        function variance(a: ArrayLike<number>, opts?: { pop?: boolean }): number;
        /** sqrt(variance) under the same opts and gates. */
        function stddev(a: ArrayLike<number>, opts?: { pop?: boolean }): number;
        /** quantile(a, 0.5); empty throws RangeError. */
        function median(a: ArrayLike<number>): number;
        /** R-7 linear quantile (DataFrame.QUANTILE's convention); q in [0, 1] and a non-empty array, else RangeError. */
        function quantile(a: ArrayLike<number>, q: number): number;
        /** Math.min semantics: NaN poisons; empty is +Infinity. */
        function min(a: ArrayLike<number>): number;
        /** Math.max semantics: NaN poisons; empty is -Infinity. */
        function max(a: ArrayLike<number>): number;
        /** Sample covariance (n-1); opts.pop divides by n; a length mismatch throws RangeError. */
        function cov(a: ArrayLike<number>, b: ArrayLike<number>, opts?: { pop?: boolean }): number;
        /** Pearson r clamped to [-1, 1]; a length mismatch or zero variance throws RangeError. */
        function corr(a: ArrayLike<number>, b: ArrayLike<number>): number;

        /** Distributions (scipy.stats semantics; values match scipy 1.18 to ~1e-9 relative, see tests/mathx/test_mathx_stats_ref.js). Each
         *  takes x as a number (returns a number) or an array-like (returns a fresh Float64Array, one value per element). Out-of-domain
         *  parameters (df <= 0, p outside [0, 1], non-integer n) give NaN rather than throwing. Tails are computed directly, never as
         *  1 - cdf, so tSf(9, 250) keeps its 2.9e-17. */
        function normPdf(x: number): number;
        function normPdf(x: ArrayLike<number>): Float64Array;
        function normCdf(x: number): number;
        function normCdf(x: ArrayLike<number>): Float64Array;
        /** Upper tail 1 - normCdf(x) without cancellation. */
        function normSf(x: number): number;
        function normSf(x: ArrayLike<number>): Float64Array;
        /** Inverse normCdf (Wichura AS241, ~1e-16 relative); 0 -> -Infinity, 1 -> Infinity. */
        function normPpf(p: number): number;
        function normPpf(p: ArrayLike<number>): Float64Array;
        /** Student t with `df` degrees of freedom (df may be fractional; Infinity is the normal). */
        function tPdf(x: number, df: number): number;
        function tPdf(x: ArrayLike<number>, df: number): Float64Array;
        function tCdf(x: number, df: number): number;
        function tCdf(x: ArrayLike<number>, df: number): Float64Array;
        function tSf(x: number, df: number): number;
        function tSf(x: ArrayLike<number>, df: number): Float64Array;
        /** Inverse tCdf (incomplete-beta inverse polished by Newton steps). */
        function tPpf(p: number, df: number): number;
        function tPpf(p: ArrayLike<number>, df: number): Float64Array;
        /** Chi-square with `k` degrees of freedom. */
        function chi2Pdf(x: number, k: number): number;
        function chi2Pdf(x: ArrayLike<number>, k: number): Float64Array;
        function chi2Cdf(x: number, k: number): number;
        function chi2Cdf(x: ArrayLike<number>, k: number): Float64Array;
        function chi2Sf(x: number, k: number): number;
        function chi2Sf(x: ArrayLike<number>, k: number): Float64Array;
        function chi2Ppf(p: number, k: number): number;
        function chi2Ppf(p: ArrayLike<number>, k: number): Float64Array;
        /** Fisher-Snedecor F with (d1, d2) degrees of freedom. */
        function fCdf(x: number, d1: number, d2: number): number;
        function fCdf(x: ArrayLike<number>, d1: number, d2: number): Float64Array;
        function fSf(x: number, d1: number, d2: number): number;
        function fSf(x: ArrayLike<number>, d1: number, d2: number): Float64Array;
        function fPpf(p: number, d1: number, d2: number): number;
        function fPpf(p: ArrayLike<number>, d1: number, d2: number): Float64Array;
        /** Binomial(n, p) at k successes; cdf/sf floor k, pmf of a non-integer k is 0; sf(k) = P(X > k). */
        function binomPmf(k: number, n: number, p: number): number;
        function binomPmf(k: ArrayLike<number>, n: number, p: number): Float64Array;
        function binomCdf(k: number, n: number, p: number): number;
        function binomCdf(k: ArrayLike<number>, n: number, p: number): Float64Array;
        function binomSf(k: number, n: number, p: number): number;
        function binomSf(k: ArrayLike<number>, n: number, p: number): Float64Array;
        /** Poisson(mu) at k events, same k conventions as the binomial. */
        function poissonPmf(k: number, mu: number): number;
        function poissonPmf(k: ArrayLike<number>, mu: number): Float64Array;
        function poissonCdf(k: number, mu: number): number;
        function poissonCdf(k: ArrayLike<number>, mu: number): Float64Array;
        function poissonSf(k: number, mu: number): number;
        function poissonSf(k: ArrayLike<number>, mu: number): Float64Array;
        /** Survival function of the Kolmogorov limit distribution (scipy.special.kolmogorov / kstwobign.sf). */
        function kolmogorovSf(x: number): number;
        function kolmogorovSf(x: ArrayLike<number>): Float64Array;

        /** Hypothesis tests (scipy.stats semantics and defaults). Samples are array-likes of finite numbers: a NaN element, an empty
         *  sample where one is needed, or a length mismatch for paired tests throws RangeError; an unknown option key throws TypeError
         *  and an unknown enum value RangeError naming it. Samples with fewer than 2 values give NaN statistic and pvalue (as scipy).
         *  ttest1samp: one-sample t test of mean(a) against `popmean` (scipy.stats.ttest_1samp); df = n - 1. */
        function ttest1samp(a: ArrayLike<number>, popmean: number, opts?: { alternative?: StatsAlternative }): StatsTestResult & { df: number };
        /** Two-sample t test of mean(a) - mean(b) (scipy.stats.ttest_ind). equalVar (default true) pools the variances with
         *  df = na + nb - 2; equalVar: false is Welch's test with Welch-Satterthwaite df. */
        function ttestInd(a: ArrayLike<number>, b: ArrayLike<number>, opts?: { alternative?: StatsAlternative; equalVar?: boolean }): StatsTestResult & { df: number };
        /** Paired t test on a[i] - b[i] (scipy.stats.ttest_rel). */
        function ttestRel(a: ArrayLike<number>, b: ArrayLike<number>, opts?: { alternative?: StatsAlternative }): StatsTestResult & { df: number };
        /** Pooled two-proportion z test of c1/n1 - c2/n2 (statsmodels proportions_ztest with value 0). A count outside [0, total] or a
         *  non-positive total throws RangeError. */
        function proportionsZTest(c1: number, n1: number, c2: number, n2: number, opts?: { alternative?: StatsAlternative }): StatsTestResult;
        /** Mann-Whitney U (scipy.stats.mannwhitneyu): statistic is U1 = number of (a, b) pairs with a > b (ties 1/2), average ranks.
         *  method 'auto' is exact (Loeffler's recurrence) when there are no ties, min(na, nb) <= 8 and na * nb <= 16384, otherwise the
         *  tie-corrected normal approximation with the 0.5 continuity correction (continuity: false drops it). An explicit 'exact'
         *  accepts na * nb <= 131072 (RangeError above) and assumes no ties. */
        function mannWhitneyU(a: ArrayLike<number>, b: ArrayLike<number>, opts?: { alternative?: StatsAlternative; method?: StatsMethod; continuity?: boolean }): StatsTestResult;
        /** Two-sample Kolmogorov-Smirnov (scipy.stats.ks_2samp): statistic is the largest ECDF gap (the signed one for less/greater).
         *  'auto' is exact (Hodges' lattice-path count) when max(na, nb) <= 10000 and falls back to asymptotic if the count overflows.
         *  'asymptotic' two-sided uses the Kolmogorov limit kolmogorovSf(sqrt(en) * D), en = na nb / (na + nb) -- scipy uses the
         *  finite-n kstwo there; one-sided uses Hodges' approximation exactly as scipy. Exact p assumes no ties. */
        function ks2Samp(a: ArrayLike<number>, b: ArrayLike<number>, opts?: { alternative?: StatsAlternative; method?: StatsMethod }): StatsTestResult;
        /** Pearson correlation r with the exact beta-distribution p (scipy.stats.pearsonr); a constant input gives NaN r and p; n = 2
         *  gives p 1 (two-sided) or 0.5. */
        function pearson(x: ArrayLike<number>, y: ArrayLike<number>, opts?: { alternative?: StatsAlternative }): StatsTestResult;
        /** Spearman rho (Pearson r of average ranks) with the t-distribution p on n - 2 df (scipy.stats.spearmanr). */
        function spearman(x: ArrayLike<number>, y: ArrayLike<number>, opts?: { alternative?: StatsAlternative }): StatsTestResult;
        /** Pearson chi-square goodness of fit (scipy.stats.chisquare); `expected` defaults to the uniform mean and must have the same
         *  total as `observed` (1e-8 relative, else RangeError); df = n - 1 - ddof. */
        function chiSquare(observed: ArrayLike<number>, expected?: ArrayLike<number> | null, opts?: { ddof?: number }): StatsTestResult & { df: number };
        /** Chi-square test of independence on an r x c table of rows (scipy.stats.chi2_contingency): Yates' correction (default on) applies
         *  only when df = 1; df = 0 gives statistic 0 and p 1. A negative cell, a ragged table or a zero row/column total throws RangeError.
         *  `expected` is the table of expected frequencies, one Float64Array per row. */
        function chi2Contingency(table: ArrayLike<ArrayLike<number>>, opts?: { correction?: boolean }): StatsTestResult & { df: number; expected: Float64Array[] };
        /** Ranks 1..n (scipy.stats.rankdata): 'average' (default), 'min', 'max', 'dense', or 'ordinal' (stable order of appearance).
         *  Values compare numerically, so -0 and +0 are equal: they tie, and 'ordinal' keeps their input order at every length. */
        function rankData(a: ArrayLike<number>, opts?: { method?: "average" | "min" | "max" | "dense" | "ordinal" }): Float64Array;
        /** Multiple-comparison adjusted p-values in input order: 'bonferroni', 'holm' (step-down), 'hochberg' (step-up), 'bh'
         *  (Benjamini-Hochberg, default) or 'by' (Benjamini-Yekutieli), each capped at 1 and monotone in the raw p (as
         *  statsmodels multipletests / scipy false_discovery_control). A p outside [0, 1] throws RangeError. */
        function pAdjust(pvalues: ArrayLike<number>, method?: "bonferroni" | "holm" | "hochberg" | "bh" | "by"): Float64Array;
        /** Population stability index between two samples: `bins` (default 10, 2..65536) quantile bins whose edges are
         *  sorted(expected)[floor((k + 1) n / bins)], a value v falling in the bin counting the edges <= v; then psiCounts. */
        function psi(expected: ArrayLike<number>, actual: ArrayLike<number>, opts?: { bins?: number; eps?: number }): number;
        /** PSI from per-bin counts: sum (pa - pe) ln(pa / pe) with each share floored at eps (default 1e-4). */
        function psiCounts(expected: ArrayLike<number>, actual: ArrayLike<number>, opts?: { eps?: number }): number;
        /** Percentile bootstrap confidence interval of mean or median(a), or with opts.b of stat(a) - stat(b) ('diff', default) or
         *  stat(a) / stat(b) ('ratio'). Resample r draws its indices from splitmix64 started at splitmix64(seed + r * 0x9E3779B97F4A7C15)
         *  (index = floor(draw * n / 2^64), a's n draws then b's), so the answer is a pure function of the inputs and seed (default 0)
         *  and identical for any thread count. low/high interpolate the sorted replicates linearly (numpy's default percentile) at
         *  (1 -+ level) / 2; a NaN replicate (0/0 ratio) makes both NaN. Resamples run on the worker pool off the JS thread above
         *  ~256K draws; the Promise rejects on bad input (RangeError/TypeError) and with InternalError "interrupted" at shutdown. */
        function bootstrap(a: ArrayLike<number>, opts?: { b?: ArrayLike<number>; statistic?: "mean" | "median"; compare?: "diff" | "ratio"; resamples?: number; level?: number; seed?: number }): Promise<{ estimate: number; low: number; high: number }>;
    }
    /** Tail of a stats hypothesis test: 'two-sided' (default), 'less' or 'greater' (the alternative that a's location is below/above b's). */
    type StatsAlternative = "two-sided" | "less" | "greater";
    /** How a stats test computes its p-value: 'auto' (default), 'exact' or 'asymptotic'. */
    type StatsMethod = "auto" | "exact" | "asymptotic";
    /** Statistic and p-value of a stats hypothesis test. */
    interface StatsTestResult {
        statistic: number;
        pvalue: number;
    }

    /** Signal processing with numpy.fft / scipy.signal semantics; values match numpy 2.5 / scipy 1.18 to ~1e-9 relative (see
     *  tests/mathx/test_mathx_signal_ref.js). Every input is a Float64Array (fast path) or any array-like of numbers capped at
     *  16777216 elements, never modified; every array result is a fresh Float64Array. Frequencies take an optional fs (default 2, so
     *  cutoffs are fractions of Nyquist as in scipy) unless stated. Transforms of any length are exact-size: lengths factoring into
     *  2, 3, 4 and odd primes <= 31 run a Stockham mixed-radix FFT, other lengths Bluestein's chirp-z over a power of two; plans
     *  (twiddles) are cached per size, shared across threads. Bad arguments throw TypeError (wrong type, unknown option key) or
     *  RangeError (out of range); nothing returns NaN for a bad shape. */
    namespace signal {
        /** Complex DFT X[k] = sum x[t] e^{-2 pi i k t / n}; im defaults to zeros. Lengths of re and im must match. */
        function fft(re: ArrayLike<number>, im?: ArrayLike<number>): { re: Float64Array; im: Float64Array };
        /** Inverse DFT with the 1/n factor, so ifft(fft(x)) == x. */
        function ifft(re: ArrayLike<number>, im: ArrayLike<number>): { re: Float64Array; im: Float64Array };
        /** The n/2 + 1 non-negative-frequency bins of a real signal's DFT (numpy.fft.rfft); n (default x.length) zero-pads or
         *  truncates. The DC bin's imaginary part is exactly 0, and the Nyquist bin's too for even n. */
        function rfft(x: ArrayLike<number>, n?: number): { re: Float64Array; im: Float64Array };
        /** Inverse of rfft to n real samples (default 2 * (re.length - 1); a one-bin spectrum needs n). Like numpy, the
         *  imaginary parts of DC and of the Nyquist bin of an even n are ignored, and the spectrum is cropped or zero-padded to
         *  n/2 + 1 bins. */
        function irfft(re: ArrayLike<number>, im: ArrayLike<number>, n?: number): Float64Array;
        /** Bin centres of an n-point fft for sample spacing d (default 1): [0, 1, ..., -2, -1] / (n d). */
        function fftfreq(n: number, d?: number): Float64Array;
        /** Bin centres of an n-point rfft: [0, 1, ..., floor(n/2)] / (n d). */
        function rfftfreq(n: number, d?: number): Float64Array;
        /** scipy.signal.get_window: periodic (DFT-even, the spectral-analysis default) unless opts.sym. 'kaiser' needs
         *  opts.beta >= 0. A 1-point window is [1]. */
        function getWindow(name: SignalWindow, n: number, opts?: { sym?: boolean; beta?: number }): Float64Array;
        /** Kaiser beta for a stopband attenuation in dB (scipy.signal.kaiser_beta). */
        function kaiserBeta(attenuationDb: number): number;
        /** Kaiser FIR design rule (scipy.signal.kaiserord): taps and beta reaching rippleDb (>= 8) with transition width as a
         *  fraction of Nyquist in (0, 1]. */
        function kaiserOrd(rippleDb: number, width: number): { numtaps: number; beta: number };
        /** Windowed-sinc FIR (scipy.signal.firwin): cutoff is one frequency or increasing band edges in (0, fs/2); passZero
         *  (default true) makes the first band a passband. window defaults to 'hamming'; scale (default true) gives unit gain at
         *  the centre of the first passband. A filter that passes Nyquist needs an odd numtaps (RangeError otherwise). */
        function firwin(numtaps: number, cutoff: number | ArrayLike<number>, opts?: { window?: SignalWindow; beta?: number; passZero?: boolean; scale?: boolean; fs?: number }): Float64Array;
        /** Direct-form II transposed IIR/FIR filter (scipy.signal.lfilter) from zero initial state, normalised by a[0] (which
         *  must be finite and non-zero). */
        function lfilter(b: ArrayLike<number>, a: ArrayLike<number>, x: ArrayLike<number>): Float64Array;
        /** Cascade of second-order sections (scipy.signal.sosfilt) from zero state. sos is flat (6 per section) or rows
         *  [b0, b1, b2, a0, a1, a2]; each a0 must be non-zero. Numerically safer than lfilter for high orders. */
        function sosfilt(sos: ArrayLike<number> | ArrayLike<ArrayLike<number>>, x: ArrayLike<number>): Float64Array;
        /** One RBJ Audio-EQ-Cookbook section [b0, b1, b2, 1, a1, a2] centred at f0 (0 < f0 < fs/2) with quality q (default
         *  SQRT1_2) and, for peaking and the shelves, gainDb (default 0; shelves use the Q form of alpha). Feed it to sosfilt. */
        function biquad(type: "lowpass" | "highpass" | "bandpass" | "notch" | "allpass" | "peaking" | "lowshelf" | "highshelf", f0: number, opts?: { q?: number; fs?: number; gainDb?: number }): Float64Array;
        /** Digital Butterworth design as flat second-order sections (scipy.signal.butter(..., output='sos'), including its
         *  'nearest' pole-zero pairing and section order): order 1..40, wn a cutoff for 'lowpass' (default) / 'highpass' or
         *  [low, high] for 'bandpass' / 'bandstop', strictly inside (0, fs/2). */
        function butter(order: number, wn: number | ArrayLike<number>, opts?: { btype?: "lowpass" | "highpass" | "bandpass" | "bandstop"; fs?: number }): Float64Array;
        /** Frequency response of b / a (a defaults to [1]) at n (default 512) points w = pi k / n, k < n; with opts.fs, w is
         *  reported in the units of fs (0 .. fs/2) instead of radians/sample. */
        function freqz(b: ArrayLike<number>, a?: ArrayLike<number> | null, opts?: { n?: number; fs?: number }): { w: Float64Array; re: Float64Array; im: Float64Array };
        /** freqz of a second-order-section cascade (same sos forms as sosfilt). */
        function sosfreqz(sos: ArrayLike<number> | ArrayLike<ArrayLike<number>>, opts?: { n?: number; fs?: number }): { w: Float64Array; re: Float64Array; im: Float64Array };
        /** Linear convolution (scipy.signal.convolve): 'full' (default, length na+nb-1), 'same' (centred, length na) or 'valid'
         *  (|na-nb|+1). Short kernels sum directly; large ones go through a 5-smooth rfft, agreeing to rounding. */
        function convolve(a: ArrayLike<number>, b: ArrayLike<number>, mode?: "full" | "same" | "valid"): Float64Array;
        /** Cross-correlation (scipy.signal.correlate): convolve(a, reversed b, mode). */
        function correlate(a: ArrayLike<number>, b: ArrayLike<number>, mode?: "full" | "same" | "valid"): Float64Array;
        /** Welch power spectral density (scipy.signal.welch, one-sided): segments of nperseg (default 256, clamped to x.length)
         *  overlapping by noverlap (default nperseg/2), periodic window (default 'hann'), each mean-removed unless detrend is
         *  false, zero-padded to nfft (default nperseg). scaling 'density' (V^2/Hz, default) or 'spectrum' (V^2). Segments are
         *  summed in fixed groups on the worker pool and reduced in order, so the result is identical for any thread count. */
        function welch(x: ArrayLike<number>, opts?: { fs?: number; window?: SignalWindow; beta?: number; nperseg?: number; noverlap?: number; nfft?: number; detrend?: "constant" | false; scaling?: "density" | "spectrum" }): { f: Float64Array; pxx: Float64Array };
        /** Analytic signal x + i H[x] (scipy.signal.hilbert) via the FFT; n (default x.length) zero-pads or truncates. The
         *  envelope is hypot(re, im). */
        function hilbert(x: ArrayLike<number>, n?: number): { re: Float64Array; im: Float64Array };
        /** One DFT bin by the Goertzel recurrence: sum x[t] e^{-2 pi i freq t / fs} (fs default 1). At freq = k fs / n this is
         *  fft(x) bin k; any real freq is allowed. O(n), no allocation. */
        function goertzel(x: ArrayLike<number>, freq: number, fs?: number): { re: number; im: number };
        /** Box Least Squares transit periodogram (Kovacs, Zucker & Mazeh 2002, binned form). y is mean-subtracted, then for each
         *  trial period the observations are phase-folded (t mod period, negative t wrapped into [0, period)) into opts.bins
         *  (default 400, integer in [2, 1048576]) equal phase bins; for each duration a box of max(1, round(duration / period *
         *  bins)) consecutive bins (wrapping the phase) slides over every start bin, and a box holding n of the N points with
         *  y-sum s scores s^2 / (N r (1 - r)), r = n / N. Boxes with fewer than opts.minPoints (default 5) points, holding every
         *  point, or as wide as the whole bin ring (duration close to or above the period) are skipped. Per period the
         *  highest-scoring box gives power, its duration, depth (-s / n: the drop below the mean of y) and epoch (box centre,
         *  mid-transit time folded into [0, period)); a period with no admissible box has power 0 and NaN duration/depth/epoch.
         *  best is the first period of maximal power, with sde = (best.power - mean(power)) / sample stddev(power) (NaN for one
         *  period or a flat periodogram); with no admissible box anywhere best.index is -1, best.power 0 and its other fields
         *  NaN. t need not be sorted (result independent of order up to rounding). Periods run in parallel on the worker pool;
         *  each period's result is deterministic. Cost O(periods * (N + durations * bins)). TypeError for a non-array input,
         *  an unknown option or a non-number option; RangeError when t and y lengths differ, there are fewer than
         *  max(2, minPoints) points, a t or y value is not finite, periods or durations is empty or holds a value that is not
         *  finite and > 0, or bins/minPoints is out of range. */
        function bls(t: ArrayLike<number>, y: ArrayLike<number>, periods: ArrayLike<number>, durations: ArrayLike<number>, opts?: { bins?: number; minPoints?: number }): { power: Float64Array; duration: Float64Array; depth: Float64Array; epoch: Float64Array; best: { index: number; period: number; duration: number; depth: number; epoch: number; power: number; sde: number } };
    }
    /** Geodesy on the sphere and the WGS84 ellipsoid. Angles are degrees (latitude in [-90, 90], longitude any finite value,
     *  results normalised to [-180, 180)), distances metres. Sphere functions use opts.radius (default 6371008.8 m, the IUGG
     *  mean radius); vincenty/vincentyDirect use WGS84 (a = 6378137, f = 1/298.257223563) and agree with a numerically
     *  integrated geodesic to 1 mm and 2e-7 deg (tests/mathx/test_mathx_geo_ref.js). Polygon rings are parallel latitude and
     *  longitude arrays, implicitly closed. Bad arguments throw TypeError (wrong type, unknown option) or RangeError. */
    namespace geo {
        /** Great-circle distance by the haversine formula. With lat2/lon2 arrays (same length) returns one distance per point as a
         *  Float64Array, computed on the worker pool for large inputs and identical to the scalar form element by element. */
        function haversine(lat1: number, lon1: number, lat2: number, lon2: number, opts?: { radius?: number }): number;
        function haversine(lat1: number, lon1: number, lat2: ArrayLike<number>, lon2: ArrayLike<number>, opts?: { radius?: number }): Float64Array;
        /** Radius search by brute-force haversine scan: the points of lat/lon within `meters` of q (distance <= meters), as
         *  ascending indices and their distances. The distances are bitwise equal to haversine(q.lat, q.lon, lat, lon) at those
         *  indices; most far points are rejected before the atan2 by comparing the haversine term itself. Runs on the worker pool
         *  for large inputs. meters must be finite and >= 0. */
        function haversineWithin(lat: ArrayLike<number>, lon: ArrayLike<number>, q: { lat: number; lon: number }, meters: number,
            opts?: { radius?: number }): { indices: Uint32Array; distances: Float64Array };
        /** Initial great-circle bearing from point 1 to point 2, degrees clockwise from north in [0, 360). */
        function bearing(lat1: number, lon1: number, lat2: number, lon2: number): number;
        /** Great-circle midpoint (normalised vector sum); for antipodal points the result is not unique. */
        function midpoint(lat1: number, lon1: number, lat2: number, lon2: number): { lat: number; lon: number };
        /** Point reached by travelling distance along the great circle leaving (lat, lon) on bearing. */
        function destination(lat: number, lon: number, bearing: number, distance: number, opts?: { radius?: number }): { lat: number; lon: number };
        /** Signed distance of (lat, lon) from the great circle through 1 then 2 (positive to the right of the direction of travel),
         *  and the distance from point 1 to the foot of the perpendicular along that circle (negative behind point 1). Throws
         *  RangeError when points 1 and 2 coincide or are antipodal. */
        function crossTrack(lat: number, lon: number, lat1: number, lon1: number, lat2: number, lon2: number, opts?: { radius?: number }): { crossTrack: number; alongTrack: number };
        /** Vincenty's inverse problem on WGS84: geodesic distance, the azimuth leaving point 1 and the forward azimuth arriving at
         *  point 2 (both [0, 360)). Coincident points give 0. Nearly antipodal pairs, where the iteration does not converge, throw
         *  RangeError rather than return a wrong answer. */
        function vincenty(lat1: number, lon1: number, lat2: number, lon2: number): { distance: number; azimuth1: number; azimuth2: number };
        /** Vincenty's direct problem on WGS84: the end point and forward azimuth after distance (0 .. 2.1e7 m) on azimuth. */
        function vincentyDirect(lat: number, lon: number, azimuth: number, distance: number): { lat: number; lon: number; azimuth: number };
        /** Base-32 geohash of a point with lon in [-180, 180], precision 1..12 characters (default 9). */
        function geohashEncode(lat: number, lon: number, precision?: number): string;
        /** Centre of a geohash cell and its half-sizes; case-insensitive; invalid characters throw RangeError. */
        function geohashDecode(hash: string): { lat: number; lon: number; latErr: number; lonErr: number };
        /** The eight same-precision neighbour cells; longitude wraps at the antimeridian, and a neighbour beyond a pole is null. */
        function geohashNeighbors(hash: string): { n: string | null; ne: string | null; e: string | null; se: string | null; s: string | null; sw: string | null; w: string | null; nw: string | null };
        /** Even-odd test of a point against a ring in the plane of (lon, lat) degrees (no antimeridian handling); points exactly on
         *  an edge may fall either way. With lat/lon arrays returns a Uint8Array of 0/1, computed on the worker pool when large. */
        function pointInPolygon(lat: number, lon: number, ringLat: ArrayLike<number>, ringLon: ArrayLike<number>): boolean;
        function pointInPolygon(lat: ArrayLike<number>, lon: ArrayLike<number>, ringLat: ArrayLike<number>, ringLon: ArrayLike<number>): Uint8Array;
        /** Area enclosed by a ring of great-circle edges on the sphere (exact spherical excess), whatever the orientation; of the
         *  two regions a ring bounds, the smaller one. */
        function polygonArea(ringLat: ArrayLike<number>, ringLon: ArrayLike<number>, opts?: { radius?: number }): number;
    }

    /** Element type of an NDArray. The codes are fixed for good (dyn-lak.h DYN_ND_*; never renumbered): float64, float32,
     *  float16, bfloat16, int32, int16, int8, uint8, bool. The backing typed array per dtype: Float64Array, Float32Array,
     *  Float16Array, Uint16Array (raw bfloat16 bits), Int32Array, Int16Array, Int8Array, Uint8Array, Uint8Array (0/1).
     *  Conversions between dtypes (astype, set, fill, assign, from) go through float64: to float32/float16/bfloat16 they round
     *  to nearest even (overflow gives +-Infinity, NaN stays NaN); to an integer type they truncate toward zero and saturate at
     *  the type's range, and NaN becomes 0; to bool any non-zero non-NaN value is 1. Block-quantized weights (q8_0, q4_0,
     *  q4_K) are not NDArray dtypes (dyna:nn owns them). Arithmetic kernels (later linalg functions) accept float64/float32. */
    type NDType = "float64" | "float32" | "float16" | "bfloat16" | "int32" | "int16" | "int8" | "uint8" | "bool";
    /** The typed array that backs an NDArray of each dtype. */
    type NDData = Float64Array | Float32Array | Float16Array | Uint16Array | Int32Array | Int16Array | Int8Array | Uint8Array;
    /** One axis of NDArray.slice: an integer picks one index and drops the axis (negative counts from the end); null or
     *  undefined keeps the whole axis; "start:stop:step" or [start, stop, step] (any part omitted or null) is a Python slice
     *  with Python's clamping, negative steps included. A step of 0 is a RangeError, a malformed string a SyntaxError. */
    type NDSlice = number | null | undefined | string | [number?, number?, number?];
    /** The strided n-dimensional array shared by mathx.linalg and dyna:nn (one type, no separate Tensor). A view is a backing
     *  typed array plus offset, shape and strides counted in ELEMENTS; a stride may be negative (a reversed axis) or 0 (a
     *  broadcast axis). At most 8 dimensions; a 0-dimensional array holds one element. Views share memory: slice, transpose,
     *  T, broadcastTo, expandDims, squeeze and (when the strides allow) reshape never copy, so a write through one view is seen
     *  by every view of the same buffer. Every access resolves the buffer afresh and never reads out of bounds: a detached
     *  ArrayBuffer, or a fixed-length typed array left out of bounds by a resize, is a TypeError; a view that no longer fits
     *  its (length-tracking) buffer is a RangeError. An NDArray built over an ArrayBuffer tracks a resizable buffer's length; writes into an immutable ArrayBuffer throw TypeError. Construction and the static makers are on the loop
     *  thread and cheap; nothing here blocks on I/O. The class is the same object as mathx.linalg.NDArray. */
    class NDArray {
        /** Wraps data without copying when it is a typed array (dtype from its class; Uint16Array needs dtype "bfloat16",
         *  Uint8Array is uint8 unless dtype "bool") or an ArrayBuffer (dtype from opts, default float64, over the whole buffer);
         *  copies a number (0-d) or a nested array of numbers (rectangular; ragged is a TypeError) into a fresh float64 (or
         *  opts.dtype) array. shape defaults to the data's own shape; strides default to C-contiguous; offset defaults to 0.
         *  A view that would reach outside the buffer is a RangeError; a dtype that does not match the typed array, an unknown
         *  option or a bad argument type is a TypeError. strides/offset need a typed array or an ArrayBuffer. */
        constructor(data: NDData | ArrayBuffer | number | number[] | number[][] | number[][][], shape?: number[], opts?: { dtype?: NDType; strides?: number[]; offset?: number });
        /** New C-contiguous array of zeros (shape may be a number for 1-D); dtype defaults to float64. */
        static zeros(shape: number | number[], dtype?: NDType): NDArray;
        /** New C-contiguous array of ones. */
        static ones(shape: number | number[], dtype?: NDType): NDArray;
        /** New C-contiguous array filled with value (converted to dtype). */
        static full(shape: number | number[], value: number, dtype?: NDType): NDArray;
        /** n x m (m defaults to n) identity-like matrix: ones on the main diagonal. */
        static eye(n: number, m?: number | null, dtype?: NDType): NDArray;
        /** numpy.arange: arange(stop) or arange(start, stop, step?); length ceil((stop - start) / step), element i is
         *  start + i * step. Non-finite arguments or step 0 are a RangeError. */
        static arange(start: number, stop?: number | null, step?: number | null, dtype?: NDType): NDArray;
        /** Always copies: an NDArray or typed array into a new C-contiguous array (keeping its dtype unless one is given), a
         *  number or nested array as the constructor does. */
        static from(data: NDArray | NDData | number | number[] | number[][] | number[][][], dtype?: NDType): NDArray;
        readonly dtype: NDType;
        readonly ndim: number;
        /** Number of elements (the product of shape; 1 for 0-d). */
        readonly size: number;
        /** A fresh copy of the shape. */
        readonly shape: number[];
        /** A fresh copy of the strides, in elements. */
        readonly strides: number[];
        /** Element offset of index (0, ..., 0) in data. */
        readonly offset: number;
        /** The whole backing typed array (shared, not a copy; the same object on every read). */
        readonly data: NDData;
        /** True when the view is C-contiguous (axes of size 1 ignored), so data.subarray(offset, offset + size) is the array. */
        readonly isContiguous: boolean;
        /** transpose() with all axes reversed (a view). */
        readonly T: NDArray;
        /** The element at the given indices (exactly ndim integers, negative from the end); bool reads as a boolean. Wrong count
         *  or an index out of range is a RangeError. */
        get(...index: number[]): number;
        /** set(i, j, ..., value) writes one element (converted to dtype); returns this. */
        set(...indexThenValue: number[]): this;
        /** A view selecting along each axis (missing trailing specs keep the axis). */
        slice(...spec: NDSlice[]): NDArray;
        /** A view with the axes permuted (all axes, each once, as arguments or one array); no arguments reverses them. */
        transpose(...axes: number[]): NDArray;
        transpose(axes: number[]): NDArray;
        /** numpy.reshape in C order: a view when the strides allow it (numpy's no-copy rule), else a contiguous copy; one
         *  dimension may be -1 (inferred). A size mismatch is a RangeError. */
        reshape(...shape: number[]): NDArray;
        reshape(shape: number[]): NDArray;
        /** A view broadcast to shape by numpy rules (new leading axes and size-1 axes get stride 0); a write through it lands on
         *  the one shared element. Incompatible shapes are a RangeError. */
        broadcastTo(shape: number[]): NDArray;
        /** A view with a new axis of size 1 inserted at axis (negative counts from the end of the result). */
        expandDims(axis: number): NDArray;
        /** A view without the size-1 axes (or only the given one, which must have size 1). */
        squeeze(axis?: number): NDArray;
        /** this when already C-contiguous, else a contiguous copy. */
        contiguous(): NDArray;
        /** A C-contiguous copy with the same dtype. */
        copy(): NDArray;
        /** A C-contiguous copy converted to dtype (see NDType for the conversion rules). */
        astype(dtype: NDType): NDArray;
        /** Writes value into every element of the view; returns this. */
        fill(value: number): this;
        /** Copies src (broadcast to this shape; converted to this dtype) into the view; overlapping source and destination
         *  views are safe (the source is read completely before writing). Returns this. */
        assign(src: NDArray | NDData | number | number[] | number[][]): this;
        /** Nested JS arrays in C order (a number for 0-d; booleans for bool). */
        toArray(): number | boolean | any[];
        /** A contiguous copy as a new typed array of the dtype's class. */
        toTypedArray(): NDData;
        /** "NDArray(float64, [2, 3])". */
        toString(): string;
    }
    /** Dense and sparse linear algebra over NDArray (numpy.linalg / scipy.linalg class). NDArray is the same class object as
     *  the top-level export. The namespace's functions are created on first access; importing dyna:mathx builds only the
     *  NDArray class. */
    namespace linalg {
        const NDArray: NDArrayClass;
    }
    /** The NDArray constructor type, so `linalg.NDArray` can name the same class. */
    type NDArrayClass = typeof NDArray;

    /** Window names accepted by signal.getWindow, firwin and welch. */
    type SignalWindow = "boxcar" | "hann" | "hamming" | "blackman" | "bartlett" | "kaiser";

    /** Compiles an arithmetic string to an RPN program; no eval, no scope. */
    class Expression {
        /** Compiles an arithmetic string to an RPN program over doubles with shunting-yard; there is no eval and no scope. */
        constructor(text: string);
        /** Free variables in first-use order. */
        variables(): string[];
        /** Evaluates, reading only own data properties. */
        eval(vars?: Record<string, number>): number;
    }
}

/** dyna:ml — models in C over contiguous doubles: linear/logistic regression, trees, forests, boosting, SVC, k-NN, k-means, DBSCAN, GMM, PCA, scalers, Pipeline, model selection. Estimators share fit/predict/serialize and are native resources; fit() refuses non-finite X (XGB* treat NaN as missing) and honours --timeout-ms.
 *  Every fit/predict/transform, metric, splitter and search returns a Promise: arguments are read and validated on the calling thread, the computation runs on a thread created for that one call and destroyed when it ends (no pool), and the result is built back on the calling thread. Bad arguments reject the promise; they never throw synchronously. `save` (encode, write, durable flush) and `load` (read, decode) are calls of the same kind. Constructors, getters, serialize/deserialize (in memory, no file I/O), `CSR.row` and `Pipeline.stage` stay synchronous. Using a model before fit() (predict, predictProba, transform, apply, featureImportances, and LogisticRegression coef/intercept/classes) rejects or throws a plain Error "<Model>.<op>: call fit() first". */
declare module "dyna:ml" {
    /** Inputs are snapshotted when the call starts: writing to, resizing or transferring X, y or weights afterwards cannot change the result. Reading nested number[][] rows happens on the calling thread; a flat Float64Array is one copy. One object, one writer: `fit`/`fitTransform` need the object to themselves and reject with TypeError while any other call on it is unsettled; reads (`predict`, `transform`, ...) may overlap each other; getters throw while a fit is in flight, and `close()` throws while any call is unsettled.
     *  At most 128 compute threads are alive at once per JS thread; a call beyond that waits its turn and still gets a thread of its own. threadStats: threads created, calls that ran inline because a thread could not be created, and threads alive now. */
    function threadStats(): { spawned: number; inline: number; live: number };
    /** A matrix as an array of rows, or a flat Float64Array plus (rows, cols). */
    type Matrix = number[][] | Float64Array;
    /** Targets or labels, one per row. */
    type Target = number[] | Float64Array;

    /** Shared estimator options. */
    interface TreeOpts {
        /** Trees (forest) or boosting rounds; default 100. Fit time is linear in it; above 100000 is refused at construction. */
        nEstimators?: number;
        /** 0 or absent means the 1024 hard ceiling; above 1024 is REFUSED (trees grow by C recursion). */
        maxDepth?: number;
        minSamplesSplit?: number;
        minSamplesLeaf?: number;
        maxFeatures?: number;
        /** 0 (default) selects the EXACT splitter; above 0 the HISTOGRAM splitter with at most this many bins per feature: much faster, slightly coarser (255 is typical). */
        maxBins?: number;
        seed?: number;
    }
    /** Trailing fit() options: `sampleWeight` gives one weight per row. */
    interface FitOpts {
        sampleWeight?: number[] | Float64Array;
    }

    /** Closed-form ordinary least squares; a CSR X gives bit-identical coefficients to the dense fit. */
    class LinearRegression implements DynResource {
        constructor();
        static deserialize(bytes: Uint8Array | ArrayBuffer): LinearRegression;
        static load(path: import("dyna:file").Path): Promise<LinearRegression>;
        /** The ridge scales with the mean weight, so scaling all weights leaves the fit unchanged. */
        fit(X: Matrix | CSR, y: Target, rows?: number, cols?: number, opts?: FitOpts): Promise<this>;
        /**: {as: "f64"} returns a flat Float64Array instead of number[]. */
        predict(X: Matrix | CSR, rows?: number, cols?: number, opts?: { as: "f64" }): Promise<number[] | Float64Array>;
        /** Writes predictions into `out` (exactly a Float64Array, not aliasing X) and returns the rows written; RangeError when `out` is too short. `out` is written only when the promise fulfils. */
        predictInto(out: Float64Array, X: Matrix | CSR, rows?: number, cols?: number): Promise<number>;
        /** Array of fitted weights (`[]` before fit). */
        readonly coef: number[];
        /** Fitted intercept (`0` before fit). */
        readonly intercept: number;
        serialize(): Uint8Array;
        save(path: import("dyna:file").Path): Promise<number>;
        close(): void;
        dispose(): void;
        readonly closed: boolean;
        readonly [Symbol.dispose]: () => void;
    }

    /** Full-batch gradient descent logistic regression. */
    class LogisticRegression implements DynResource {
        /** Defaults: learningRate 0.1, maxIter 3000, tol 1e-4, l1 = l2 = 0; `C` is the inverse penalty strength used with `penalty` (scikit-learn's convention). */
        constructor(opts?: { learningRate?: number; maxIter?: number; tol?: number; l1?: number; l2?: number; C?: number; penalty?: "l1" | "l2" | "elasticnet" | "none"; classWeight?: "balanced" });
        static deserialize(bytes: Uint8Array | ArrayBuffer): LogisticRegression;
        static load(path: import("dyna:file").Path): Promise<LogisticRegression>;
        fit(X: Matrix | CSR, y: Target, rows?: number, cols?: number, opts?: FitOpts): Promise<this>;
        /**: {as: "f64"} returns a flat Float64Array instead of number[]. */
        predict(X: Matrix | CSR, rows?: number, cols?: number, opts?: { as: "f64" }): Promise<number[] | Float64Array>;
        /** Writes predictions into `out` (exactly a Float64Array, not aliasing X) and returns the rows written; RangeError when `out` is too short. `out` is written only when the promise fulfils. */
        predictInto(out: Float64Array, X: Matrix | CSR, rows?: number, cols?: number): Promise<number>;
        /** Class probabilities, one row per sample. */
        predictProba(X: Matrix | CSR, rows?: number, cols?: number): Promise<number[][]>;
        /** Distinct labels, ascending. */
        readonly classes: number[];
        /** Weight matrix (1 row binary, one per class multinomial). */
        readonly coef: number[][];
        /** Scalar (binary) or Array (multinomial). */
        readonly intercept: number | number[];
        /** Iterations run. */
        readonly nIter: number;
        /** Whether the gradient-norm stop fired. */
        readonly converged: boolean;
        serialize(): Uint8Array;
        save(path: import("dyna:file").Path): Promise<number>;
        close(): void;
        dispose(): void;
        readonly closed: boolean;
        readonly [Symbol.dispose]: () => void;
    }

    /** Lloyd's algorithm with k-means++ seeding. */
    class KMeans implements DynResource {
        /** nClusters defaults to 8. A negative seed keeps the non-reproducible default; pass a non-negative integer (or `{ seed }`) for reproducibility. */
        constructor(nClusters?: number, seed?: number | { seed?: number });
        static deserialize(bytes: Uint8Array | ArrayBuffer): KMeans;
        static load(path: import("dyna:file").Path): Promise<KMeans>;
        fit(X: Matrix, rows?: number, cols?: number, opts?: FitOpts): Promise<this>;
        /**: {as: "f64"} returns a flat Float64Array instead of number[]. */
        predict(X: Matrix | CSR, rows?: number, cols?: number, opts?: { as: "f64" }): Promise<number[] | Float64Array>;
        /** Writes predictions into `out` (exactly a Float64Array, not aliasing X) and returns the rows written; RangeError when `out` is too short. `out` is written only when the promise fulfils. */
        predictInto(out: Float64Array, X: Matrix | CSR, rows?: number, cols?: number): Promise<number>;
        /** Summed squared distance to assigned centroids of the last fit (`0` before fit). */
        readonly inertia: number;
        serialize(): Uint8Array;
        save(path: import("dyna:file").Path): Promise<number>;
        close(): void;
        dispose(): void;
        readonly closed: boolean;
        readonly [Symbol.dispose]: () => void;
    }

    /** SMO support-vector classifier. */
    class SVC implements DynResource {
        /** Defaults: kernel "rbf", C 1, gamma 1/cols, coef0 0, degree 3, tol 1e-3, maxIter 1000; more than two classes train one-vs-rest (up to 256). */
        constructor(opts?: { kernel?: "linear" | "rbf" | "poly"; C?: number; gamma?: number; coef0?: number; degree?: number; tol?: number; maxIter?: number });
        static deserialize(bytes: Uint8Array | ArrayBuffer): SVC;
        static load(path: import("dyna:file").Path): Promise<SVC>;
        /** Needs at least two distinct labels. */
        fit(X: Matrix, y: Target, rows?: number, cols?: number): Promise<this>;
        /**: {as: "f64"} returns a flat Float64Array instead of number[]. */
        predict(X: Matrix | CSR, rows?: number, cols?: number, opts?: { as: "f64" }): Promise<number[] | Float64Array>;
        /** Writes predictions into `out` (exactly a Float64Array, not aliasing X) and returns the rows written; RangeError when `out` is too short. `out` is written only when the promise fulfils. */
        predictInto(out: Float64Array, X: Matrix | CSR, rows?: number, cols?: number): Promise<number>;
        /** A FLAT Float64Array comes back when X is flat. */
        decisionFunction(X: Matrix | CSR, rows?: number, cols?: number): Promise<number[] | number[][] | Float64Array>;
        /** Total support vectors across all binary machines (`0` before fit). */
        readonly nSupportVectors: number;
        /** Labels in ascending order. */
        readonly classes: number[];
        serialize(): Uint8Array;
        save(path: import("dyna:file").Path): Promise<number>;
        close(): void;
        dispose(): void;
        readonly closed: boolean;
        readonly [Symbol.dispose]: () => void;
    }

    /** EM soft clustering over diagonal-covariance Gaussians. */
    class GaussianMixture implements DynResource {
        /** Defaults: k 3, seed 12345, maxIter 200, tol 1e-3, regCovar 1e-6 (added to every variance so a component cannot collapse). */
        constructor(k?: number, opts?: { seed?: number; maxIter?: number; tol?: number; regCovar?: number });
        static deserialize(bytes: Uint8Array | ArrayBuffer): GaussianMixture;
        static load(path: import("dyna:file").Path): Promise<GaussianMixture>;
        /** Needs at least `k` rows. */
        fit(X: Matrix, rows?: number, cols?: number): Promise<this>;
        /**: {as: "f64"} returns a flat Float64Array instead of number[]. */
        predict(X: Matrix | CSR, rows?: number, cols?: number, opts?: { as: "f64" }): Promise<number[] | Float64Array>;
        /** Writes predictions into `out` (exactly a Float64Array, not aliasing X) and returns the rows written; RangeError when `out` is too short. `out` is written only when the promise fulfils. */
        predictInto(out: Float64Array, X: Matrix | CSR, rows?: number, cols?: number): Promise<number>;
        /** A FLAT Float64Array comes back when X is flat. */
        predictProba(X: Matrix | CSR, rows?: number, cols?: number): Promise<number[][] | Float64Array>;
        /** Mixing weights (sum 1). */
        readonly weights: number[];
        readonly means: number[][];
        /** K×cols, regularised. */
        readonly variances: number[][];
        /** Total log-likelihood of the last fit (`0` before fit). */
        readonly logLikelihood: number;
        /** EM iterations run. */
        readonly nIter: number;
        serialize(): Uint8Array;
        save(path: import("dyna:file").Path): Promise<number>;
        close(): void;
        dispose(): void;
        readonly closed: boolean;
        readonly [Symbol.dispose]: () => void;
    }

    /** Naive Bayes with per-class Gaussian densities. */
    class GaussianNB implements DynResource {
        /** varSmoothing defaults to 1e-9, scaled by the largest variance (scikit-learn's floor). */
        constructor(varSmoothing?: number);
        static deserialize(bytes: Uint8Array | ArrayBuffer): GaussianNB;
        static load(path: import("dyna:file").Path): Promise<GaussianNB>;
        fit(X: Matrix, y: Target, rows?: number, cols?: number, opts?: FitOpts): Promise<this>;
        /**: {as: "f64"} returns a flat Float64Array instead of number[]. */
        predict(X: Matrix | CSR, rows?: number, cols?: number, opts?: { as: "f64" }): Promise<number[] | Float64Array>;
        /** Writes predictions into `out` (exactly a Float64Array, not aliasing X) and returns the rows written; RangeError when `out` is too short. `out` is written only when the promise fulfils. */
        predictInto(out: Float64Array, X: Matrix | CSR, rows?: number, cols?: number): Promise<number>;
        /** A FLAT Float64Array comes back when X is flat. */
        predictProba(X: Matrix | CSR, rows?: number, cols?: number): Promise<number[][] | Float64Array>;
        /** Labels seen at fit. */
        readonly classes: number[];
        serialize(): Uint8Array;
        save(path: import("dyna:file").Path): Promise<number>;
        close(): void;
        dispose(): void;
        readonly closed: boolean;
        readonly [Symbol.dispose]: () => void;
    }

    /** CART classifier over Gini impurity. */
    class DecisionTreeClassifier implements DynResource {
        /** TreeOpts defaults: maxDepth 0 (unlimited up to 1024), minSamplesSplit 2, minSamplesLeaf 1, maxBins 0 (exact), seed 12345; a single tree ignores nEstimators. */
        constructor(opts?: TreeOpts);
        static deserialize(bytes: Uint8Array | ArrayBuffer): DecisionTreeClassifier;
        static load(path: import("dyna:file").Path): Promise<DecisionTreeClassifier>;
        fit(X: Matrix, y: Target, rows?: number, cols?: number, opts?: FitOpts): Promise<this>;
        /**: {as: "f64"} returns a flat Float64Array instead of number[]. */
        predict(X: Matrix | CSR, rows?: number, cols?: number, opts?: { as: "f64" }): Promise<number[] | Float64Array>;
        /** Writes predictions into `out` (exactly a Float64Array, not aliasing X) and returns the rows written; RangeError when `out` is too short. `out` is written only when the promise fulfils. */
        predictInto(out: Float64Array, X: Matrix | CSR, rows?: number, cols?: number): Promise<number>;
        /** Class probabilities, one row per sample. */
        predictProba(X: Matrix | CSR, rows?: number, cols?: number): Promise<number[][]>;
        /** The leaf index each sample lands in, per tree. */
        apply(X: Matrix, rows?: number, cols?: number): Promise<number[][]>;
        /** Gini-importance per feature, normalised to sum 1 (zeros when nothing ever split). */
        readonly featureImportances: number[];
        /** Deepest tree (0 before fit). */
        readonly depth: number;
        serialize(): Uint8Array;
        save(path: import("dyna:file").Path): Promise<number>;
        close(): void;
        dispose(): void;
        readonly closed: boolean;
        readonly [Symbol.dispose]: () => void;
    }

    /** CART regressor minimising variance. */
    class DecisionTreeRegressor implements DynResource {
        constructor(opts?: TreeOpts);
        static deserialize(bytes: Uint8Array | ArrayBuffer): DecisionTreeRegressor;
        static load(path: import("dyna:file").Path): Promise<DecisionTreeRegressor>;
        fit(X: Matrix, y: Target, rows?: number, cols?: number, opts?: FitOpts): Promise<this>;
        /**: {as: "f64"} returns a flat Float64Array instead of number[]. */
        predict(X: Matrix | CSR, rows?: number, cols?: number, opts?: { as: "f64" }): Promise<number[] | Float64Array>;
        /** Writes predictions into `out` (exactly a Float64Array, not aliasing X) and returns the rows written; RangeError when `out` is too short. `out` is written only when the promise fulfils. */
        predictInto(out: Float64Array, X: Matrix | CSR, rows?: number, cols?: number): Promise<number>;
        /** The leaf index each sample lands in, per tree. */
        apply(X: Matrix, rows?: number, cols?: number): Promise<number[][]>;
        /** Gini-importance per feature. */
        readonly featureImportances: number[];
        /** Deepest tree (0 before fit). */
        readonly depth: number;
        serialize(): Uint8Array;
        save(path: import("dyna:file").Path): Promise<number>;
        close(): void;
        dispose(): void;
        readonly closed: boolean;
        readonly [Symbol.dispose]: () => void;
    }

    /** Bagged decision trees; a fixed seed reproduces a forest exactly. */
    class RandomForestClassifier implements DynResource {
        /** TreeOpts defaults: nEstimators 100, maxDepth 0, minSamplesSplit 2, minSamplesLeaf 1, maxBins 0 (exact), seed 12345. */
        constructor(opts?: TreeOpts);
        static deserialize(bytes: Uint8Array | ArrayBuffer): RandomForestClassifier;
        static load(path: import("dyna:file").Path): Promise<RandomForestClassifier>;
        fit(X: Matrix, y: Target, rows?: number, cols?: number, opts?: FitOpts): Promise<this>;
        /**: {as: "f64"} returns a flat Float64Array instead of number[]. */
        predict(X: Matrix | CSR, rows?: number, cols?: number, opts?: { as: "f64" }): Promise<number[] | Float64Array>;
        /** Writes predictions into `out` (exactly a Float64Array, not aliasing X) and returns the rows written; RangeError when `out` is too short. `out` is written only when the promise fulfils. */
        predictInto(out: Float64Array, X: Matrix | CSR, rows?: number, cols?: number): Promise<number>;
        /** Class probabilities, one row per sample. */
        predictProba(X: Matrix | CSR, rows?: number, cols?: number): Promise<number[][]>;
        /** The leaf index each sample lands in, per tree. */
        apply(X: Matrix, rows?: number, cols?: number): Promise<number[][]>;
        /** Gini-importance per feature. */
        readonly featureImportances: number[];
        readonly depth: number;
        serialize(): Uint8Array;
        save(path: import("dyna:file").Path): Promise<number>;
        close(): void;
        dispose(): void;
        readonly closed: boolean;
        readonly [Symbol.dispose]: () => void;
    }

    /** Bagged regression trees. */
    class RandomForestRegressor implements DynResource {
        constructor(opts?: TreeOpts);
        static deserialize(bytes: Uint8Array | ArrayBuffer): RandomForestRegressor;
        static load(path: import("dyna:file").Path): Promise<RandomForestRegressor>;
        fit(X: Matrix, y: Target, rows?: number, cols?: number, opts?: FitOpts): Promise<this>;
        /**: {as: "f64"} returns a flat Float64Array instead of number[]. */
        predict(X: Matrix | CSR, rows?: number, cols?: number, opts?: { as: "f64" }): Promise<number[] | Float64Array>;
        /** Writes predictions into `out` (exactly a Float64Array, not aliasing X) and returns the rows written; RangeError when `out` is too short. `out` is written only when the promise fulfils. */
        predictInto(out: Float64Array, X: Matrix | CSR, rows?: number, cols?: number): Promise<number>;
        /** The leaf index each sample lands in, per tree. */
        apply(X: Matrix, rows?: number, cols?: number): Promise<number[][]>;
        /** Gini-importance per feature. */
        readonly featureImportances: number[];
        readonly depth: number;
        serialize(): Uint8Array;
        save(path: import("dyna:file").Path): Promise<number>;
        close(): void;
        dispose(): void;
        readonly closed: boolean;
        readonly [Symbol.dispose]: () => void;
    }

    /** First-order boosting for regression. Shared options (nEstimators <= 100000, maxDepth <= 1024, maxBins, seed) follow TreeOpts. */
    class GradientBoostingRegressor implements DynResource {
        /** Defaults: nEstimators 100, maxDepth 3, learningRate 0.1, subsample 1, seed 12345. */
        constructor(opts?: { nEstimators?: number; maxDepth?: number; learningRate?: number; subsample?: number; minSamplesSplit?: number; minSamplesLeaf?: number; maxFeatures?: number; maxBins?: number; seed?: number });
        static deserialize(bytes: Uint8Array | ArrayBuffer): GradientBoostingRegressor;
        static load(path: import("dyna:file").Path): Promise<GradientBoostingRegressor>;
        fit(X: Matrix, y: Target, rows?: number, cols?: number, opts?: FitOpts): Promise<this>;
        /**: {as: "f64"} returns a flat Float64Array instead of number[]. */
        predict(X: Matrix | CSR, rows?: number, cols?: number, opts?: { as: "f64" }): Promise<number[] | Float64Array>;
        /** Writes predictions into `out` (exactly a Float64Array, not aliasing X) and returns the rows written; RangeError when `out` is too short. `out` is written only when the promise fulfils. */
        predictInto(out: Float64Array, X: Matrix | CSR, rows?: number, cols?: number): Promise<number>;
        /** The leaf index each sample lands in, per tree. */
        apply(X: Matrix, rows?: number, cols?: number): Promise<number[][]>;
        /** Gini-importance per feature. */
        readonly featureImportances: number[];
        readonly depth: number;
        serialize(): Uint8Array;
        save(path: import("dyna:file").Path): Promise<number>;
        close(): void;
        dispose(): void;
        readonly closed: boolean;
        readonly [Symbol.dispose]: () => void;
    }

    /** First-order boosting for classification. Shared options (nEstimators <= 100000, maxDepth <= 1024, maxBins, seed) follow TreeOpts. */
    class GradientBoostingClassifier implements DynResource {
        /** Defaults: nEstimators 100, maxDepth 3, learningRate 0.1, subsample 1, seed 12345. */
        constructor(opts?: { nEstimators?: number; maxDepth?: number; learningRate?: number; subsample?: number; minSamplesSplit?: number; minSamplesLeaf?: number; maxFeatures?: number; maxBins?: number; seed?: number });
        static deserialize(bytes: Uint8Array | ArrayBuffer): GradientBoostingClassifier;
        static load(path: import("dyna:file").Path): Promise<GradientBoostingClassifier>;
        fit(X: Matrix, y: Target, rows?: number, cols?: number, opts?: FitOpts): Promise<this>;
        /**: {as: "f64"} returns a flat Float64Array instead of number[]. */
        predict(X: Matrix | CSR, rows?: number, cols?: number, opts?: { as: "f64" }): Promise<number[] | Float64Array>;
        /** Writes predictions into `out` (exactly a Float64Array, not aliasing X) and returns the rows written; RangeError when `out` is too short. `out` is written only when the promise fulfils. */
        predictInto(out: Float64Array, X: Matrix | CSR, rows?: number, cols?: number): Promise<number>;
        /** Class probabilities, one row per sample. */
        predictProba(X: Matrix | CSR, rows?: number, cols?: number): Promise<number[][]>;
        /** The leaf index each sample lands in, per tree. */
        apply(X: Matrix, rows?: number, cols?: number): Promise<number[][]>;
        /** Gini-importance per feature. */
        readonly featureImportances: number[];
        readonly depth: number;
        serialize(): Uint8Array;
        save(path: import("dyna:file").Path): Promise<number>;
        close(): void;
        dispose(): void;
        readonly closed: boolean;
        readonly [Symbol.dispose]: () => void;
    }

    /** Second-order (Newton) boosting; NaN means "missing". */
    class XGBRegressor implements DynResource {
        /** Defaults: nEstimators 100, maxDepth 6, learningRate 0.3, lambda 1, alpha 0, gamma 0, minChildWeight 1, validationFraction 0.1, earlyStoppingRounds 0. */
        constructor(opts?: { nEstimators?: number; maxDepth?: number; learningRate?: number; subsample?: number; colsampleByTree?: number; lambda?: number; alpha?: number; gamma?: number; minChildWeight?: number; validationFraction?: number; earlyStoppingRounds?: number; maxBins?: number; seed?: number });
        static deserialize(bytes: Uint8Array | ArrayBuffer): XGBRegressor;
        static load(path: import("dyna:file").Path): Promise<XGBRegressor>;
        fit(X: Matrix, y: Target, rows?: number, cols?: number, opts?: FitOpts): Promise<this>;
        /**: {as: "f64"} returns a flat Float64Array instead of number[]. */
        predict(X: Matrix | CSR, rows?: number, cols?: number, opts?: { as: "f64" }): Promise<number[] | Float64Array>;
        /** Writes predictions into `out` (exactly a Float64Array, not aliasing X) and returns the rows written; RangeError when `out` is too short. `out` is written only when the promise fulfils. */
        predictInto(out: Float64Array, X: Matrix | CSR, rows?: number, cols?: number): Promise<number>;
        /** The leaf index each sample lands in, per tree. */
        apply(X: Matrix, rows?: number, cols?: number): Promise<number[][]>;
        /** Gini-importance per feature. */
        readonly featureImportances: number[];
        readonly depth: number;
        /** The rounds kept, equal to `nEstimators` without early stopping, else the best validation round (0 before fit). */
        readonly bestRounds: number;
        serialize(): Uint8Array;
        save(path: import("dyna:file").Path): Promise<number>;
        close(): void;
        dispose(): void;
        readonly closed: boolean;
        readonly [Symbol.dispose]: () => void;
    }

    /** The classifier twin of XGBRegressor. */
    class XGBClassifier implements DynResource {
        /** Defaults: nEstimators 100, maxDepth 6, learningRate 0.3, lambda 1, alpha 0, gamma 0, minChildWeight 1, validationFraction 0.1, earlyStoppingRounds 0. */
        constructor(opts?: { nEstimators?: number; maxDepth?: number; learningRate?: number; subsample?: number; colsampleByTree?: number; lambda?: number; alpha?: number; gamma?: number; minChildWeight?: number; validationFraction?: number; earlyStoppingRounds?: number; maxBins?: number; seed?: number });
        static deserialize(bytes: Uint8Array | ArrayBuffer): XGBClassifier;
        static load(path: import("dyna:file").Path): Promise<XGBClassifier>;
        fit(X: Matrix, y: Target, rows?: number, cols?: number, opts?: FitOpts): Promise<this>;
        /**: {as: "f64"} returns a flat Float64Array instead of number[]. */
        predict(X: Matrix | CSR, rows?: number, cols?: number, opts?: { as: "f64" }): Promise<number[] | Float64Array>;
        /** Writes predictions into `out` (exactly a Float64Array, not aliasing X) and returns the rows written; RangeError when `out` is too short. `out` is written only when the promise fulfils. */
        predictInto(out: Float64Array, X: Matrix | CSR, rows?: number, cols?: number): Promise<number>;
        /** Class probabilities, one row per sample. */
        predictProba(X: Matrix | CSR, rows?: number, cols?: number): Promise<number[][]>;
        /** The leaf index each sample lands in, per tree. */
        apply(X: Matrix, rows?: number, cols?: number): Promise<number[][]>;
        /** Gini-importance per feature. */
        readonly featureImportances: number[];
        readonly depth: number;
        /** The rounds kept, equal to `nEstimators` without early stopping, else the best validation round (0 before fit). */
        readonly bestRounds: number;
        serialize(): Uint8Array;
        save(path: import("dyna:file").Path): Promise<number>;
        close(): void;
        dispose(): void;
        readonly closed: boolean;
        readonly [Symbol.dispose]: () => void;
    }

    /** Principal components by cyclic Jacobi; fit() refuses (RangeError) when 60 * nFeatures^3 exceeds 2^36 (~1100 features). */
    class PCA implements DynResource {
        /** nComponents 0 (default) keeps every feature; `whiten` defaults to false. */
        constructor(nComponents?: number, whiten?: boolean);
        static deserialize(bytes: Uint8Array | ArrayBuffer): PCA;
        static load(path: import("dyna:file").Path): Promise<PCA>;
        fit(X: Matrix, rows?: number, cols?: number): Promise<this>;
        /** Centred rows projected onto the components. */
        transform(X: Matrix, rows?: number, cols?: number): Promise<number[][]>;
        /** Writes the transformed rows into `out` (exactly a Float64Array, not aliasing X) and returns the row count; RangeError when `out` is too short. `out` is written only when the promise fulfils. */
        transformInto(out: Float64Array, X: Matrix, rows?: number, cols?: number): Promise<number>;
        /** A FLAT Float64Array comes back when X is a flat Float64Array (rows x cols); nested input yields number[][]. */
        fitTransform(X: Matrix, rows?: number, cols?: number): Promise<number[][] | Float64Array>;
        /** The projection back into feature space (used with `nComponents` < features to reconstruct). */
        inverseTransform(X: Matrix, rows?: number, cols?: number): Promise<number[][]>;
        /** NComponents × features (unit rows). */
        readonly components: number[][];
        /** Per-feature training mean. */
        readonly mean: number[];
        /** Eigenvalues of the sample (n-1) covariance, retained components. */
        readonly explainedVariance: number[];
        /** Each divided by the total variance (sums to ~1). */
        readonly explainedVarianceRatio: number[];
        serialize(): Uint8Array;
        save(path: import("dyna:file").Path): Promise<number>;
        close(): void;
        dispose(): void;
        readonly closed: boolean;
        readonly [Symbol.dispose]: () => void;
    }

    /** k-nearest-neighbours classifier; lazy, distances stay squared. */
    class KNClassifier implements DynResource {
        /** k defaults to 5; weights is "uniform" (default) or "distance". */
        constructor(k?: number, weights?: "uniform" | "distance");
        static deserialize(bytes: Uint8Array | ArrayBuffer): KNClassifier;
        static load(path: import("dyna:file").Path): Promise<KNClassifier>;
        /** Requires at least `k` rows. */
        fit(X: Matrix, y: Target, rows?: number, cols?: number): Promise<this>;
        /**: {as: "f64"} returns a flat Float64Array instead of number[]. */
        predict(X: Matrix | CSR, rows?: number, cols?: number, opts?: { as: "f64" }): Promise<number[] | Float64Array>;
        /** Writes predictions into `out` (exactly a Float64Array, not aliasing X) and returns the rows written; RangeError when `out` is too short. `out` is written only when the promise fulfils. */
        predictInto(out: Float64Array, X: Matrix | CSR, rows?: number, cols?: number): Promise<number>;
        serialize(): Uint8Array;
        save(path: import("dyna:file").Path): Promise<number>;
        close(): void;
        dispose(): void;
        readonly closed: boolean;
        readonly [Symbol.dispose]: () => void;
    }

    /** k-nearest-neighbours regressor. */
    class KNRegressor implements DynResource {
        /** k defaults to 5; weights is "uniform" (default) or "distance". */
        constructor(k?: number, weights?: "uniform" | "distance");
        static deserialize(bytes: Uint8Array | ArrayBuffer): KNRegressor;
        static load(path: import("dyna:file").Path): Promise<KNRegressor>;
        /** Requires at least `k` rows. */
        fit(X: Matrix, y: Target, rows?: number, cols?: number): Promise<this>;
        /**: {as: "f64"} returns a flat Float64Array instead of number[]. */
        predict(X: Matrix | CSR, rows?: number, cols?: number, opts?: { as: "f64" }): Promise<number[] | Float64Array>;
        /** Writes predictions into `out` (exactly a Float64Array, not aliasing X) and returns the rows written; RangeError when `out` is too short. `out` is written only when the promise fulfils. */
        predictInto(out: Float64Array, X: Matrix | CSR, rows?: number, cols?: number): Promise<number>;
        serialize(): Uint8Array;
        save(path: import("dyna:file").Path): Promise<number>;
        close(): void;
        dispose(): void;
        readonly closed: boolean;
        readonly [Symbol.dispose]: () => void;
    }

    /** Density-based clustering with a grid index for low-dimensional data. */
    class DBScan implements DynResource {
        /** eps defaults to 0.5, minPts to 5. */
        constructor(eps?: number, minPts?: number);
        static deserialize(bytes: Uint8Array | ArrayBuffer): DBScan;
        static load(path: import("dyna:file").Path): Promise<DBScan>;
        /** No weighted form; `sampleWeight` throws. */
        fit(X: Matrix, rows?: number, cols?: number): Promise<this>;
        /** Array of per-row cluster ids, `-1` for noise (`[]` before fit). */
        readonly labels: number[];
        /** Cluster count (0 before fit). */
        readonly nClusters: number;
        /** The eps the model was constructed with. */
        readonly eps: number;
        serialize(): Uint8Array;
        save(path: import("dyna:file").Path): Promise<number>;
        close(): void;
        dispose(): void;
        readonly closed: boolean;
        readonly [Symbol.dispose]: () => void;
    }

    /** Per-column z-score scaling; constant columns report std 1.0. */
    class StandardScaler implements DynResource {
        constructor();
        static deserialize(bytes: Uint8Array | ArrayBuffer): StandardScaler;
        static load(path: import("dyna:file").Path): Promise<StandardScaler>;
        fit(X: Matrix, rows?: number, cols?: number, opts?: FitOpts): Promise<this>;
        /** A FLAT Float64Array comes back when X is a flat Float64Array (rows x cols); nested input yields number[][]. */
        transform(X: Matrix, rows?: number, cols?: number): Promise<number[][] | Float64Array>;
        /** Writes the transformed rows into `out` (exactly a Float64Array, not aliasing X) and returns the row count; RangeError when `out` is too short. `out` is written only when the promise fulfils. */
        transformInto(out: Float64Array, X: Matrix, rows?: number, cols?: number): Promise<number>;
        /** A FLAT Float64Array comes back when X is flat. */
        fitTransform(X: Matrix, rows?: number, cols?: number): Promise<number[][] | Float64Array>;
        /** `x·std + mean`. Output shape mirrors the input shape. */
        inverseTransform(X: Matrix, rows?: number, cols?: number): Promise<number[][]>;
        /** Per-feature means. */
        readonly mean: number[];
        /** Per-feature standard deviations (`1.0` for constant columns). */
        readonly std: number[];
        serialize(): Uint8Array;
        save(path: import("dyna:file").Path): Promise<number>;
        close(): void;
        dispose(): void;
        readonly closed: boolean;
        readonly [Symbol.dispose]: () => void;
    }

    /** Per-column min-max scaling to [0, 1]. */
    class MinMaxScaler implements DynResource {
        constructor();
        static deserialize(bytes: Uint8Array | ArrayBuffer): MinMaxScaler;
        static load(path: import("dyna:file").Path): Promise<MinMaxScaler>;
        /** Refuses `sampleWeight` (min and max are order statistics no positive weight can change). */
        fit(X: Matrix, rows?: number, cols?: number): Promise<this>;
        /** A FLAT Float64Array comes back when X is flat. */
        transform(X: Matrix, rows?: number, cols?: number): Promise<number[][] | Float64Array>;
        /** Writes the transformed rows into `out` (exactly a Float64Array, not aliasing X) and returns the row count; RangeError when `out` is too short. `out` is written only when the promise fulfils. */
        transformInto(out: Float64Array, X: Matrix, rows?: number, cols?: number): Promise<number>;
        /** A FLAT Float64Array comes back when X is flat. */
        fitTransform(X: Matrix, rows?: number, cols?: number): Promise<number[][] | Float64Array>;
        /** `x·(max−min) + min`. Output shape mirrors the input shape. */
        inverseTransform(X: Matrix, rows?: number, cols?: number): Promise<number[][]>;
        /** Per-column minima. */
        readonly dataMin: number[];
        /** Per-column maxima. */
        readonly dataMax: number[];
        serialize(): Uint8Array;
        save(path: import("dyna:file").Path): Promise<number>;
        close(): void;
        dispose(): void;
        readonly closed: boolean;
        readonly [Symbol.dispose]: () => void;
    }

    /** An immutable compressed sparse row matrix (scipy layout). Every predict* accepts one and matches the dense answer bit for bit; only Linear/LogisticRegression fit one.
     *  Duplicate columns in a row SUM; the ctor validates everything: TypeError on shape/length mismatch, RangeError otherwise. */
    class CSR implements DynResource {
        constructor(values: number[] | Float64Array, columns: number[] | Int32Array | Uint32Array, rowPointers: number[] | Int32Array | Uint32Array, cols: number);
        /** Drops exact zeros from a dense matrix; a non-finite value is a RangeError naming the cell. */
        static fromDense(X: Matrix, rows?: number, cols?: number): Promise<CSR>;
        /** The dense form (duplicate columns sum); throws if it does not fit in memory. */
        toDense(): Promise<number[][]>;
        /** Row i as a dense Array (duplicate columns sum). */
        row(i: number): number[];
        readonly rows: number;
        readonly cols: number;
        /** Nonzero count. */
        readonly nnz: number;
        /** `nnz / (rows·cols)`. */
        readonly density: number;
        close(): void;
        dispose(): void;
        readonly closed: boolean;
        readonly [Symbol.dispose]: () => void;
    }

    /** DBScan is NOT a PipelineStage: it fits but cannot transform/predict, so a Pipeline has no position for it. */
    type PipelineStage = LinearRegression | LogisticRegression | KMeans | SVC
        | GaussianMixture | GaussianNB | DecisionTreeClassifier | DecisionTreeRegressor
        | RandomForestClassifier | RandomForestRegressor | GradientBoostingRegressor
        | GradientBoostingClassifier | XGBRegressor | XGBClassifier | PCA
        | KNClassifier | KNRegressor | StandardScaler | MinMaxScaler;

    /** A composition of feature stages and a final estimator. */
    class Pipeline implements DynResource {
        constructor(stages: PipelineStage[]);
        fit(X: Matrix, y: Target): Promise<this>;
        /** Pushes X through the stages, ask the last one. */
        predict(X: Matrix): Promise<number[]>;
        /** Class probabilities, one row per sample. */
        predictProba(X: Matrix): Promise<number[][]>;
        /** Pushes through every stage that can transform, stopping before a final bare estimator. */
        transform(X: Matrix): Promise<number[][]>;
        /** The i-th stage (negative counts from the end). */
        stage(i: number): PipelineStage;
        readonly length: number;
        readonly fitted: boolean;
        /** The final stage. */
        readonly estimator: PipelineStage;
        close(): void;
        dispose(): void;
        readonly closed: boolean;
        readonly [Symbol.dispose]: () => void;
    }

    /** Mean of squared errors. */
    function meanSquaredError(yTrue: Target, yPred: Target): Promise<number>;
    /** The mean absolute error. */
    function meanAbsoluteError(yTrue: Target, yPred: Target): Promise<number>;
    /** Coefficient of determination; a constant yTrue scores 1.0 for exact, else 0.0. */
    function r2Score(yTrue: Target, yPred: Target): Promise<number>;
    /** The fraction of equal pairs. */
    function accuracy(yTrue: Target, yPred: Target): Promise<number>;
    /** Mean negative log-likelihood; yPred may be a class matrix or a binary vector. */
    function logLoss(yTrue: Target, yPred: Target | Target[]): Promise<number>;
    /** Confusion matrix indexed [true][pred]; labels are integers 0..4095 (dense nLabels x nLabels). */
    function confusionMatrix(yTrue: Target, yPred: Target): Promise<number[][]>;
    /** Binary-classification metrics; `positive` names the positive class label. */
    function precision(yTrue: Target, yPred: Target, positive?: number): Promise<number>;
    /** The recall. Defaults: positive 1. */
    function recall(yTrue: Target, yPred: Target, positive?: number): Promise<number>;
    /** The F1 score. Defaults: positive 1. */
    function f1(yTrue: Target, yPred: Target, positive?: number): Promise<number>;
    /** The specificity. Defaults: positive 1. */
    function specificity(yTrue: Target, yPred: Target, positive?: number): Promise<number>;
    /** `(recall + specificity)/2`. Defaults: positive 1. */
    function balancedAccuracy(yTrue: Target, yPred: Target, positive?: number): Promise<number>;
    /** The Matthews correlation coefficient, the one number that stays honest on imbalanced data. Defaults: positive 1. */
    function matthewsCorrcoef(yTrue: Target, yPred: Target, positive?: number): Promise<number>;
    /** Agreement over chance. Defaults: positive 1. */
    function cohenKappa(yTrue: Target, yPred: Target, positive?: number): Promise<number>;
    /** beta > 1 weights recall. */
    function fbeta(yTrue: Target, yPred: Target, beta: number, positive?: number): Promise<number>;
    /** Exact Mann-Whitney U; lacking a positive or negative example is a RangeError; `positive` defaults to the largest label. */
    function rocAuc(yTrue: Target, yScore: Target, positive?: number): Promise<number>;
    /** Area under the precision-recall curve, from scores. */
    function averagePrecision(yTrue: Target, yScore: Target, positive?: number): Promise<number>;

    /** Indices, not data. testSize is a FRACTION in (0,1), default 0.25; the count form (testSize: 10) is a RangeError. */
    function trainTestSplit(n: number | Target, opts?: { testSize?: number; shuffle?: boolean; seed?: number }): Promise<{ train: number[]; test: number[] }>;
    /** Default 5 folds; when both `k` and `folds` are given, `folds` wins. */
    function kFold(n: number | Target, opts?: { k?: number; folds?: number; shuffle?: boolean; seed?: number }): Promise<{ train: number[]; test: number[] }[]>;
    /** K folds that preserve each class's proportion: each class's members are dealt round-robin to folds 0..k-1 in row order,
     *  or, with shuffle, in the order of a seeded permutation, so different seeds give different folds (shuffle:false is seed-independent). */
    function stratifiedKFold(y: Target, opts?: { k?: number; shuffle?: boolean; seed?: number }): Promise<{ train: number[]; test: number[] }[]>;
    /** Cross-validated scores, one per fold; estimatorFactory is () => new Model(...). `nJobs` is validated but folds run one after another (each fit and predict on its own thread); `onProgress({done, total})` fires after each fold. */
    function crossValScore(estimatorFactory: () => { fit(X: Matrix, y: Target): unknown; predict(X: Matrix): number[]; close(): void }, X: Matrix, y: Target, opts?: { k?: number; seed?: number; scoring?: (yTrue: number[], yPred: number[]) => number; nJobs?: number; onProgress?: (p: { done: number; total: number }) => void }): Promise<number[]>;
    /** Exhaustive parameter search. */
    function gridSearch(estimatorFactory: (params: Record<string, unknown>) => unknown, X: Matrix, y: Target, grid: Record<string, unknown[]>, opts?: { k?: number; seed?: number; scoring?: (yTrue: number[], yPred: number[]) => number; nJobs?: number; onProgress?: (p: { done: number; total: number }) => void }): Promise<{ best: Record<string, unknown>; bestScore: number; results: { params: Record<string, unknown>; scores: number[]; mean: number }[] }>;
    /** Random parameter search over nIter sampled points. */
    function randomSearch(estimatorFactory: (params: Record<string, unknown>) => unknown, X: Matrix, y: Target, grid: Record<string, unknown[]>, opts?: { nIter?: number; k?: number; seed?: number; scoring?: (yTrue: number[], yPred: number[]) => number; nJobs?: number; onProgress?: (p: { done: number; total: number }) => void }): Promise<{ best: Record<string, unknown>; bestScore: number; results: { params: Record<string, unknown>; scores: number[]; mean: number }[] }>;

    /** Replaces every non-finite entry with its column's finite mean; a column with NO finite value is a RangeError naming it. */
    function imputeMean(X: Matrix, rows?: number, cols?: number): Promise<number[][]>;
    /** Removes rows holding a non-finite value; `kept` lists the survivors. */
    function dropMissing(X: Matrix, y?: Target, rows?: number, cols?: number): Promise<{ X: number[][]; y: Float64Array | undefined; kept: number[] }>;
}
/** dyna:random — a seedable xoshiro256** PRNG with distributions, shuffle/sample and checkpointable state; NOT for secrets (use crypto.RandomBytes). */
declare module "dyna:random" {
    /** A seedable xoshiro256** PRNG; a given seed is deterministic and reproducible. */
    class Random {
        /** The seed coerces via ToInt64 ("42" and 42n give the 42 stream); omitted draws from OS entropy. */
        constructor(seed?: number | bigint);
        /** A full 64-bit draw, always BigInt. */
        nextU64(): bigint;
        /** The top 53 bits as an exact Number in [0, 2^53). */
        nextU53(): number;
        /** A double in [0, 1). */
        nextFloat(): number;
        /** Uniform in [0, bound) by rejection sampling (no modulo bias); the result type mirrors the argument; a Number bound must be an integer in [1, 2^53]. */
        nextBounded(bound: number): number;
        nextBounded(bound: bigint): bigint;
        /** Fills any typed array with random bytes over an optional (offset, length) window in ELEMENTS; returns `this`; DataView/ArrayBuffer are refused. */
        fill(typedArray: AnyTypedArray, offset?: number, length?: number): this;
        /** Fills the whole array with N(mu, sigma) draws (both values of each polar pair are used); f32 lanes are rounded once. */
        fillNormal(buf: Float32Array | Float64Array, mu?: number, sigma?: number): this;
        /** Fills with uniform draws in [lo, hi) (default [0, 1)); a float32 value that rounds up to hi is stepped back below it. */
        fillUniform(buf: Float32Array | Float64Array, lo?: number, hi?: number): this;
        /** Fills with exponential draws of rate lambda (default 1), by inversion as exponential() does. */
        fillExponential(buf: Float32Array | Float64Array, lambda?: number): this;
        /** n fresh random bytes; n must be an integer in [0, 2^30] (RangeError otherwise, before any allocation or draw).
         *  The keystream is filled straight into the result's engine-heap buffer (no scratch copy: peak memory is n, not 2n; ~5 GB/s fill), so the result counts toward the engine memory limit, not setNativeMemoryLimit. */
        bytes(n: number): Uint8Array;
        /** Normal(mu = 0, sigma = 1) by Marsaglia's polar method with no cached second draw, so state checkpoints replay exactly. */
        normal(mu?: number, sigma?: number): number;
        /** Exponential(lambda = 1) by inverse CDF; lambda must be finite and > 0. */
        exponential(lambda?: number): number;
        /** Exact Poisson(lambda) for lambda in [0, 2^31-1]; returns a non-negative integer. */
        poisson(lambda: number): number;
        /** Gamma(shape, scale = 1) by Marsaglia-Tsang (shape < 1 boosted by U^(1/shape)); both must be finite and > 0. */
        gamma(shape: number, scale?: number): number;
        /** Beta(a, b) in [0, 1] as X / (X + Y) of two gamma draws (in log space when a shape is below 1, so tiny shapes give 0 or 1, never NaN). */
        beta(a: number, b: number): number;
        /** Exact Binomial(n, p) by inversion searched outward from the mode (one uniform per draw); n is an integer in [0, 2^31-1], p in [0, 1]. */
        binomial(n: number, p: number): number;
        /** In-place Fisher-Yates from THIS instance's stream (so it is reproducible); returns `this`; frozen collections are refused. */
        shuffle(arr: AnyTypedArray | unknown[]): this;
        /** n elements WITHOUT replacement; the source is not mutated; a typed array yields its own type. */
        sample(arr: AnyTypedArray | unknown[], n: number): AnyTypedArray | unknown[];
        /** One uniform element; an empty array is a RangeError. */
        choice(arr: AnyTypedArray | unknown[]): unknown;
        /** Advances the state by 2^128 draws without generating them: same-seed generators separated by a jump give disjoint streams. */
        jump(): this;
        /** longJump: the 2^192-advance variant, for 2^64 independent lanes. */
        longJump(): this;
        /** A 32-byte opaque snapshot of the generator state (four LE u64 words). */
        getState(): Uint8Array;
        /** Restores a state captured by getState(); an all-zero state is the xoshiro fixed point and is refused. */
        setState(state: Uint8Array): void;
    }
}

/** dyna:schema — JSON Schema (Draft 2020-12) validation: a schema compiles once into a native tree, validation is pure dispatch. */
declare module "dyna:schema" {
    /** JSON Schema Draft 2020-12 validation; a referenced resource declaring an older draft's $schema is still processed under 2020-12 rules. */
    interface SchemaError {
        path: string;
        message: string;
        keyword: string;
    }
    /** The outcome of validate(); `errors` lists the failed keywords. */
    interface SchemaResult {
        valid: boolean;
        errors: SchemaError[];
    }
    /** A compiled schema: reusable, thread-safe, pure dispatch at validate time. */
    interface CompiledSchema {
        /** Validates an instance against the compiled schema. */
        validate(instance: unknown): SchemaResult;
    }
    const Schema: {
        /** Compiles once (Draft 2020-12): $id base URIs, $anchor, $dynamicRef dynamic scope, the embedded 2020-12 meta-schemas, $vocabulary from a meta-schema in resources.
         *  resources maps absolute URI -> schema (nothing is fetched); formats: "registered" (default: only registerFormat names assert), "annotation" or "assertion" (built-ins too). */
        compile(schema: unknown, opts?: { resources?: Record<string, unknown>; baseUri?: string; formats?: "registered" | "annotation" | "assertion" }): CompiledSchema;
        /** Compiles and caches on the schema object; accepts an already-compiled schema. `pattern` and `patternProperties` run under their own budget of 100M backtracking steps plus 1000 per subject character (RegExp itself has none), so a catastrophic pattern from an untrusted schema throws RangeError("dyna:schema: pattern execution timed out") instead of hanging. */
        validate(schema: unknown | CompiledSchema, instance: unknown): SchemaResult;
        /** Registers a custom "format" validator for string instances, live for already-compiled schemas; per runtime; duplicate names are refused. */
        registerFormat(name: string, fn: (value: string) => unknown): void;
    };
}

/** dyna:scrape — polite crawling: robots.txt policy, per-host pacing, jittered retries, conditional GETs, body caps, field extraction, resumable Crawl, sitemaps.
 *  Works over dyna:html's element tree; Fetcher gates every redirect hop (private hosts, https downgrades). */
declare module "dyna:scrape" {
    /** robots.txt parsing (RFC 9309). */
    class Robots implements DynResource {
        /** Parses a robots.txt body (RFC 9309). */
        constructor(text: string, opts?: { agent?: string });
        /** True when the path is allowed for the configured agent. */
        allows(path: string): boolean;
        /** The crawl delay for the agent (the largest across its combined groups), or null when unset. */
        crawlDelay(): number | null;
        /** The sitemap URLs listed in the file. */
        sitemaps(): string[];
        /** The number of rules that survived parsing in the groups applying to the agent; every group naming it (or every "*" group when none does) is combined (RFC 9309 2.2.1). */
        readonly ruleCount: number;
        close(): void;
        dispose(): void;
        readonly closed: boolean;
        readonly [Symbol.dispose]: () => void;
    }

    /** Field extraction spec: a selector per field. GC-managed: no close surface. */
    class Extractor {
        /** `spec` maps field names to { sel, attr?, all?, required?, trim?, source?, default?, as? } with as: "number" | "url" | "json". */
        constructor(spec: Record<string, unknown>, opts?: { text?: (node: import("dyna:html").HTMLElement) => string });
        /** Runs against a parsed dyna:html document; options.base resolves `as: "url"` fields. */
        run(doc: import("dyna:html").HTMLElement | import("dyna:html").HTMLElement[], opts?: { base?: string }): { ok: boolean; value: Record<string, unknown>; missing: string[] };
    }

    /** Polite HTTP retrieval with robots policy, retries and backoff. Asynchronous only (2026-11-26 owner decision, a clean break: the hidden synchronous HTTPClient hook is gone): get() and getStream() return Promises and never block the JS thread, so a Fetcher can crawl an App or HTTPServer in the same process. Argument errors throw synchronously: a missing url, a closed Fetcher, a url that is not http(s) or longer than 1023 bytes, and a private/loopback/link-local host when allowPrivateHosts is off. Everything after that rejects: transport errors once retries are spent, redirect gates (too many hops, an https downgrade, a private redirect target), header validation, `ca` loading, maxBodyBytes, and closing the Fetcher while a fetch is in flight (TypeError "Fetcher: closed while get() was in flight"). Pacing is reserved up front: concurrent get()s to one host each claim the next minDelayMs slot (stats().throttledMs sums the waits), and robots.txt is fetched once per host however many requests wait on it. */
    class Fetcher implements DynResource {
        constructor(opts: {
            agent: string;
            /** Optional transport: omitted, the Fetcher builds its own HTTPClient capped at maxBodyBytes; inject one (or a mock) to drive the policy in tests. */
            client?: unknown;
            /** A proxy URL: validated, stored, and redacted in stats(); the built-in transport connects directly, so routing through it is an injected client's job. */
            proxy?: string;
            /** A CA bundle path or inline PEM. With the built-in transport it REPLACES the platform trust store for https, in get(), getStream() and Crawl alike; it is loaded at the first https request, so a missing file or unreadable PEM throws there (TypeError "Fetcher: ca: ..."). An injected `client` keeps its own trust. stats() redacts it. */
            ca?: string;
            /** Connection pool size 1..64 (default 4): validated and echoed; the built-in transport opens one connection per request. */
            poolSize?: number;
            /** A dyna:http CookieJar for the built-in transport: get(), getStream() and Crawl send its cookies and store every response's Set-Cookie (cookies follow their own Domain/Path rules, unlike a Cookie header, which is refused). With an injected `client` it is a TypeError: call client.useCookies(jar) instead. */
            cookieJar?: import("dyna:http").CookieJar;
            /** Extra request headers; User-Agent stays the crawler's own, and credential headers (Authorization/Cookie/...) are refused: they would leak to redirect targets. */
            headers?: Record<string, string>;
            /** Conditional GETs against stored validators. Default true. */
            revalidate?: boolean;
            /** robots.txt refresh interval in ms (default 24h; 0 re-fetches per request); a failed refresh keeps the last good copy. */
            robotsTtlMs?: number;
            /** Follow a redirect that moves https to http. Default false. */
            allowInsecureDowngrade?: boolean;
            /** Default true: robots.txt is fetched, cached and enforced. */
            robots?: boolean;
            /** Per-host politeness floor, ms. Default 1000. */
            minDelayMs?: number;
            /** Retries per request on 429/5xx/transport errors. Default 3. */
            retries?: number;
            /** Redirect hops chased. Default 5. Only 301/302/303/307/308 WITH a Location are redirects (fetch standard 4.4): a 304, a 300, or any 3xx without Location is returned as the response, never counted against this budget. A Location resolving back to the same url is returned too. */
            maxRedirects?: number;
            /** Response-body cap in bytes. Default 8 MiB. */
            maxBodyBytes?: number;
            /** Default false: private/loopback/link-local targets refused by the SSRF gate; true allows them. An IP literal is judged by its address alone: IPv6 ::, ::1, fe80::/10, fc00::/7, ff00::/8, 2001::/23, 2001:db8::/32, 64:ff9b:1::/48 and ::ffff:0:0/96 or 64:ff9b::/96 embedding a private IPv4 are refused, any other IPv6 address is fetched. */
            allowPrivateHosts?: boolean;
            /** retries/maxRedirects/maxBodyBytes/minDelayMs/robotsTtlMs must fit int32 (RangeError beyond, never truncated). */
        });
        /** GET a URL; resolves with the HTTP response with url/fromCache/notModified added. An injected `client` needs request(method, url, body, headers) returning the response or a Promise of it. */
        get(url: string): Promise<FetcherResponse>;
        /** The crawl statistics; `savedBytes` sums cached-body sizes served on revalidation. */
        stats(): {
            fetched: number; skippedByRobots: number; retried: number;
            throttledMs: number; bytes: number; revalidated: number; savedBytes: number;
            /** The stored simple options, redacted: proxy userinfo becomes `***@`, a PEM `ca` becomes `<pem-redacted>`. */
            proxy: string | null; ca: string | null; poolSize: number;
        };
        /** Fetches under get()'s policy (robots, pacing, retries, redirect gates, maxBodyBytes) but returns a streaming FetcherStream; no revalidation or response decorations. Shares dyna:http's non-blocking transport and parser (the stream is an HTTPClient StreamedResponse with the final url): chunk sizes are strict 1*HEXDIG (RFC 9112 7.1: " 5", "+5", "0x5" reject the read), header lines are validated as in HTTPClient, and https verifies against `ca` or the platform store. It always uses a built-in HTTPClient (an injected `client` is not used for streams) with a 15 s per-read timeout, `cookieJar` and `ca`; with allowPrivateHosts off its onConnect refuses a connection whose resolved peer address is private (a DNS-rebinding guard; it rejects). A declared Content-Length over maxBodyBytes rejects with RangeError "Fetcher: declared Content-Length ... exceeds maxBodyBytes"; a transport failure once retries are spent rejects with RangeError "Fetcher: no response (...) after N attempt(s)". A robots-refused url resolves with a status-0 stream already at its end. */
        getStream(url: string): Promise<FetcherStream>;
        close(): void;
        dispose(): void;
        readonly closed: boolean;
        readonly [Symbol.dispose]: () => void;
    }

    /** A fetched page; on revalidation the status stays 304 with the cached body and its stored content-type. */
    interface FetcherResponse {
        status: number;
        statusText: string;
        /** status is 200..299. */
        ok: boolean;
        headers: Record<string, string>;
        contentType: string;
        /** The body decoded as text; there is no `text` field. */
        body: string;
        /** The body as received, before text decoding (an ArrayBuffer, not a view). */
        bodyBytes: ArrayBuffer;
        url: string;
        fromCache: boolean;
        notModified: boolean;
        /** X-Robots-Tag directives that apply to this fetcher's agent. */
        robotsDirectives?: string[];
        /** The `Link: rel="canonical"` target (RFC 8288), resolved against the request url. */
        canonicalUrl?: string;
    }

    /** The getStream response: head fields plus a read(buf)/close() body, consumed directly by lines() and ndjson(), or as a ReadableStream through toReadableStream(body). */
    interface FetcherStream {
        status: number;
        statusText: string;
        ok: boolean;
        headers: Record<string, string>;
        /** The final url after redirect chasing. */
        url: string;
        contentType: string;
        /** Present (true) on a robots-refused fetch: status 0, body at EOF. */
        skippedByRobots?: boolean;
        /** Fills the caller's buffer and resolves with the count; 0 = end of body. Rejects on truncation, maxBodyBytes overflow ("HTTPBodyStream: body exceeds maxBodyBytes") or malformed chunking. */
        read(buf: Uint8Array): Promise<number>;
        close(): void;
        dispose(): void;
        readonly closed: boolean;
        readonly [Symbol.dispose]: () => void;
    }

    /** One page emitted by a Crawl. */
    interface CrawlPage {
        /** The page URL (seed and relative hrefs resolved). */
        url: string;
        /** Crawl depth of this page (seed is depth 0). */
        depth: number;
        /** HTTP status of the fetch. */
        status: number;
        /** The Extractor's fields for this page. */
        value: Record<string, unknown>;
        /** Merged X-Robots-Tag + meta-robots directive list, when either exists. */
        robots?: string[];
    }

    /** Bounded traversal over a Fetcher plus an Extractor; relative links resolve against the page that emitted them. */
    class Crawl implements DynResource {
        constructor(fetcher: Fetcher, opts?: {
            /** Defaults: maxPages 100, maxDepth 2, sameHost true; all three serialize()/resume(). */
            maxPages?: number; maxDepth?: number; sameHost?: boolean;
            linkField?: string; baseField?: string;
            /** rel=canonical dedup: duplicate pages emit but queue no links. */
            canonicalField?: string;
            /** Parallel array of link rel strings; nofollow slots are skipped. */
            relField?: string;
            /** Extractor field holding the page's <meta name="robots"> content: its nofollow gates link following. */
            robotsField?: string;
            /** Pages fetched in parallel (1..16, default 1). Per-host politeness is unchanged. */
            concurrency?: number;
        });
        /** Seeds the crawl with an http(s) url; `parse` is the document parser, normally dyna:html's HTMLParse. */
        start(seed: string, extractor?: Extractor, parse?: (html: string) => import("dyna:html").HTMLElement[]): this;
        /** One page per call, always a Promise (2026-11-26: the synchronous concurrency-1 path and the sync iterator were removed with the synchronous Fetcher); drain with for await. Each page is fetched with the Fetcher's get() (an object with a get(url) method works too), so revalidation applies, and page.url is the final url after redirects, the base for its relative links. A fetch failure rejects that next(); closing the Fetcher mid-crawl rejects with TypeError "Crawl: the fetcher was closed while the crawl was in flight". */
        next(): Promise<IteratorResult<CrawlPage>>;
        /** The crawl itself as a lazy async page iterator: one fetch per next(), never collected eagerly. */
        pages(): AsyncIterableIterator<CrawlPage>;
        /** Async iteration: for await (const page of crawl). */
        [Symbol.asyncIterator](): AsyncIterator<CrawlPage>;
        /** The resumable state as versioned JSON: frontier, visited keys, bounds, counters. In-flight fetches and live objects are not part of it. */
        serialize(): string;
        close(): void;
        dispose(): void;
        readonly closed: boolean;
        readonly [Symbol.dispose]: () => void;
        /** Rebuilds a crawl from serialize() output with fresh fetcher/extractor/parse; the remaining pages and their order match the donor's. */
        static resume(fetcher: Fetcher, state: string, extractor?: Extractor,
                      parse?: (html: string) => import("dyna:html").HTMLElement[]): Crawl;
    }

    /** Sitemap reader for the URLs Robots.sitemaps hands out. */
    namespace Sitemap {
        /** Parses sitemap XML (or a <sitemapindex>) into its <loc> list in document order. Bounds: 16 MiB, 50000 entries, 2048-byte <loc>. */
        function parse(xml: string): string[];
        /** Fetch + parse + flatten, following a <sitemapindex> ONE level; asynchronous (resolves with the urls). Rejects with RangeError on non-2xx, more than 1000 child sitemaps, or more than 50000 URLs in total. `source` is a Fetcher (or any object with an async get(url)), or an options bag for a Fetcher of its own, which is closed when the list settles; a bad options bag throws synchronously. */
        function list(url: string, source?: Fetcher | {
            agent?: string; headers?: Record<string, string>;
            minDelayMs?: number; allowPrivateHosts?: boolean;
            maxBodyBytes?: number; retries?: number;
        }): Promise<string[]>;
    }
}

/** dyna:svg — SVG 1.1 to raster: renders an SVG document to PNG bytes or raw RGBA pixels, natively, with no external libraries.
 *  Both functions are asynchronous only: each call copies its input, then parses and rasterises on the shared worker pool (one
 *  render per pool thread, so parallel calls spread across the pool) and settles its Promise on the event loop (it runs as a fiber offloaded to the pool like dyna:async offload: calls beyond the pool queue wait in order, and with --io-threads 0 the render runs inline on the loop thread, settling the same way); there are no
 *  synchronous variants. Every failure is a rejection, never a synchronous throw: an input that is not a string or Uint8Array,
 *  a bad option type, or a document with no <svg> root rejects with TypeError; an out-of-range option, or an image or document
 *  over the built-in resource limits (64 Mpx output, 200000 nodes and others), rejects with RangeError; elements nested deeper than 1024 levels are not drawn.
 *  Supported: shapes, paths, transforms, viewBox and preserveAspectRatio, units, solid paint, linear and radial gradients with
 *  spread modes, patterns, clipPath, mask, opacity, markers, <use>/<symbol>, nested <svg>, <switch>, CSS (<style>, inline style,
 *  selectors), text in a built-in bitmap font (tspan, textPath, rotate, anchors), the SVG 1.1 filter primitives, and data: URI
 *  images (PNG, JPEG, SVG). External files are never read: an image href that is not a data: URI draws nothing.
 *  Rendering uses 4x supersampled anti-aliasing; output pixels are non-premultiplied RGBA8, rows top to bottom. */
declare module "dyna:svg" {
    /** Render options. width/height: output size in px, integers 1..65536 (one alone keeps the aspect ratio; neither = the
     *  document's own size). scale: extra uniform scale, > 0 and <= 65536 (default 1). background: fill painted under the
     *  drawing, "#RRGGBB" or "#RRGGBBAA" (the "#" is optional) or an integer 0xRRGGBBAA; omitted = transparent. */
    interface RenderOptions {
        width?: number;
        height?: number;
        scale?: number;
        background?: string | number;
    }
    /** Renders `svg` (the document text, or its UTF-8 bytes) to a PNG image: 8-bit RGBA, non-interlaced, zlib-compressed. */
    function render(svg: string | Uint8Array, opts?: RenderOptions): Promise<Uint8Array>;
    /** Renders `svg` to raw pixels: `data` holds width*height*4 bytes, non-premultiplied RGBA, row-major from the top-left. */
    function renderRGBA(svg: string | Uint8Array, opts?: RenderOptions): Promise<{ width: number; height: number; data: Uint8Array }>;
}

/** dyna:webrtc — W3C WebRTC (webrtc-pc) peer connections and data channels, natively in C: ICE (RFC 8445, full agent,
 *  STUN RFC 8489, TURN-over-UDP RFC 8656 client), DTLS 1.2 through the linked OpenSSL (RFC 6347, use_srtp RFC 5764,
 *  certificate fingerprints RFC 8122), SCTP over DTLS (RFC 9260 / RFC 8261, partial reliability RFC 3758, stream reset
 *  RFC 6525) and DCEP (RFC 8832). The same class objects are installed as globals (globalThis.RTCPeerConnection ===
 *  import("dyna:webrtc").RTCPeerConnection), created on first use. Signaling is the caller's: exchange
 *  localDescription / icecandidate events over any channel. Media negotiation (webrtc-pc 5, JSEP RFC 8829): transceivers, senders, receivers,
 *  tracks and streams, offer/answer with audio and video m-sections in one BUNDLE group (RFC 9143) beside the data
 *  m-section; RTP/SRTP media flow is not wired yet. One transport per connection: an answer rejects (port 0) an
 *  accepted m-section outside the offer's BUNDLE group. A re-offer keeps every payload type and extmap id negotiated
 *  before (RFC 8829 5.2.2): a codec or extension the remote numbered keeps that number, and a default number the remote
 *  gave to something else moves to a free dynamic one (rtx apt follows). A simulcast offer (a=rid send +
 *  a=simulcast:send, RFC 8853) is answered with a=rid recv and a=simulcast:recv for the rids it lists when the answer
 *  receives; as the offerer, applying an answer removes the encodings whose rid its a=simulcast:recv omits and keeps
 *  only the first encoding when the answer has no simulcast. A remote offer asking us to send simulcast
 *  (a=simulcast:recv) is answered without simulcast. Rollback of a remote re-offer restores the remote stream
 *  membership of every receiver track and the direction, currentDirection and stopped state of a transceiver the offer
 *  stopped (what the application changed meanwhile is kept); a receiver track the offer ended stays ended (a track
 *  never returns to "live").
 *  Design choices: one UDP socket per gathered interface on the native batched UDP layer, each owning 32 receive buffers
 *  of 2 KiB (datagrams are processed in place) and 4 MiB kernel buffers, so a full 1 MiB SCTP window burst on loopback is
 *  not dropped; receiving never allocates per packet (fixed rings, one reassembly buffer per association, out-of-order
 *  chunks in recycled 1280-byte slots, at most 256 cached per association); send() copies each message once into the
 *  SCTP queue; a DATA chunk that fills a gap is SACKed at once (RFC 9260 6.7), so a reopened window is announced without
 *  the 200 ms delayed-SACK wait; a live RTCPeerConnection keeps the event loop alive until close() (or until the
 *  engine shuts down, which closes every connection); events are delivered as macrotasks after the operation that
 *  caused them has settled. */
declare module "dyna:webrtc" {
    type RTCSdpType = "offer" | "pranswer" | "answer" | "rollback";
    type RTCSignalingState = "stable" | "have-local-offer" | "have-remote-offer" | "have-local-pranswer" | "have-remote-pranswer" | "closed";
    type RTCIceGatheringState = "new" | "gathering" | "complete";
    type RTCRtpTransceiverDirection = "sendrecv" | "sendonly" | "recvonly" | "inactive" | "stopped";
    type RTCPriorityType = "very-low" | "low" | "medium" | "high";

    /** dyna extension, not part of webrtc-pc: the optional second constructor argument. A strict bag: an unknown key is a
     *  TypeError naming the valid ones; anything but an object is a TypeError. Spec behaviour never depends on it; it only
     *  moves the safety caps below. */
    interface RTCPeerConnectionDynaOptions {
        /** Caps applied while parsing every remote description and building every local one. Each value is a positive
         *  integer, or Infinity to remove the cap (it then stops only at 2^24 entries, 2^30 bytes for the two byte caps);
         *  a non-number is a TypeError, 0, a fraction or a value above that ceiling is a RangeError. A description over a
         *  cap is refused whole (OperationError, errorDetail "sdp-syntax-error"), never truncated.
         *  Defaults: maxInputBytes 1048576, maxLineBytes 4096, maxMediaSections 64, maxCodecsPerSection 128,
         *  maxHeaderExtensions 64 (per m= section), maxCandidatesPerSection 64, maxSsrcsPerSection 64,
         *  maxMsidsPerSection 16, maxSsrcGroups 16 (per m= section), maxSsrcsPerGroup 16, maxUnknownLines 1024 (per
         *  description). Raising them costs memory per description only: every parsed list is sized by what the
         *  description holds, not by the cap. */
        sdpLimits?: {
            maxInputBytes?: number;
            maxLineBytes?: number;
            maxMediaSections?: number;
            maxCodecsPerSection?: number;
            maxHeaderExtensions?: number;
            maxCandidatesPerSection?: number;
            maxSsrcsPerSection?: number;
            maxMsidsPerSection?: number;
            maxSsrcGroups?: number;
            maxSsrcsPerGroup?: number;
            maxUnknownLines?: number;
        };
    }

    /** A track handle (mediacapture-main), no capture devices: tracks come from RTCRtpReceiver.track or clone(), and
     *  carry no media in this module. new MediaStreamTrack() is a TypeError. A remote track starts muted; it is unmuted
     *  by nothing here (media flow is not wired), muted again when the remote side stops sending, and ended (with an
     *  "ended" event) when its transceiver is stopped by negotiation; stop() ends it without an event, a
     *  local transceiver stop() ends it at once and fires "ended" in a later task, close() ends it without an event. contentHint accepts "", "speech", "speech-recognition", "music" (audio) or "", "motion",
     *  "detail", "text" (video); other values are ignored. getCapabilities/getConstraints return {}, getSettings
     *  returns { deviceId }, applyConstraints resolves. */
    class MediaStreamTrack extends EventTarget {
        readonly kind: "audio" | "video";
        /** A UUID v4. */
        readonly id: string;
        readonly label: string;
        enabled: boolean;
        readonly muted: boolean;
        readonly readyState: "live" | "ended";
        contentHint: string;
        clone(): MediaStreamTrack;
        stop(): void;
        getCapabilities(): Record<string, unknown>;
        getConstraints(): Record<string, unknown>;
        getSettings(): Record<string, unknown>;
        applyConstraints(constraints?: Record<string, unknown>): Promise<void>;
        onended: ((ev: Event) => void) | null;
        onmute: ((ev: Event) => void) | null;
        onunmute: ((ev: Event) => void) | null;
    }

    /** mediacapture-main MediaStream: an ordered set of tracks (adding one twice is a no-op). addTrack/removeTrack fire
     *  nothing; addtrack/removetrack events ({ track }) fire only for remote streams, when a remote description adds a
     *  track to the stream (a=msid) or removes it. Remote streams are one object per stream id per connection. */
    class MediaStream extends EventTarget {
        constructor(init?: MediaStream | MediaStreamTrack[]);
        readonly id: string;
        /** True while any track is not ended. */
        readonly active: boolean;
        getTracks(): MediaStreamTrack[];
        getAudioTracks(): MediaStreamTrack[];
        getVideoTracks(): MediaStreamTrack[];
        getTrackById(id: string): MediaStreamTrack | null;
        addTrack(track: MediaStreamTrack): void;
        removeTrack(track: MediaStreamTrack): void;
        /** A new stream (new id) of clones of every track. */
        clone(): MediaStream;
        onaddtrack: ((ev: Event & { track: MediaStreamTrack }) => void) | null;
        onremovetrack: ((ev: Event & { track: MediaStreamTrack }) => void) | null;
    }

    interface RTCRtpCodec {
        mimeType: string;
        clockRate: number;
        channels?: number;
        sdpFmtpLine?: string;
    }
    interface RTCRtpCodecParameters extends RTCRtpCodec {
        payloadType: number;
    }
    /** The built-in table: audio opus/48000/2 (minptime=10;useinbandfec=1), PCMU, PCMA, telephone-event/48000 and
     *  telephone-event/8000 (RFC 4733, fmtp 0-15); video VP8, H264 profile-level-id
     *  42e01f and 42001f (packetization-mode=1), each with RTX (listed once as "video/rtx", no fmtp); header extensions
     *  sdes:mid and transport-wide-cc (both kinds), ssrc-audio-level (audio), rtp-stream-id and repaired-rtp-stream-id
     *  (video). */
    interface RTCRtpCapabilities {
        codecs: RTCRtpCodec[];
        headerExtensions: { uri: string }[];
    }
    interface RTCRtpEncodingParameters {
        /** RFC 8851 rid: 1..255 of [A-Za-z0-9_-]; read-only after addTransceiver. */
        rid?: string;
        active?: boolean;
        maxBitrate?: number;
        /** Video only (dropped for audio); >= 0, else RangeError. */
        maxFramerate?: number;
        /** Video only (dropped for audio); >= 1, else RangeError. Default 2^(n-i-1) for encoding i of n when no encoding
         *  sets it, 1 otherwise. */
        scaleResolutionDownBy?: number;
        priority?: RTCPriorityType;
        networkPriority?: RTCPriorityType;
        /** The codec to send this encoding with (webrtc-pc 5.2). addTransceiver: one not in
         *  RTCRtpSender.getCapabilities(kind).codecs is an OperationError. setParameters: it must match a negotiated
         *  codec, else (before negotiation) a codec preference other than RTX/RED/FEC, else a capability; otherwise
         *  InvalidModificationError. Applying an answer removes it when the answer did not keep that codec. */
        codec?: RTCRtpCodec;
    }
    interface RTCRtpParameters {
        /** The negotiated header extensions (empty until an answer is applied). */
        headerExtensions: { uri: string; id: number; encrypted: boolean }[];
        rtcp: { cname?: string; reducedSize?: boolean };
        /** The negotiated codecs (empty until an answer is applied). */
        codecs: RTCRtpCodecParameters[];
    }
    interface RTCRtpSendParameters extends RTCRtpParameters {
        transactionId: string;
        encodings: RTCRtpEncodingParameters[];
    }
    interface RTCRtpTransceiverInit {
        /** "stopped" is a TypeError. */
        direction?: RTCRtpTransceiverDirection;
        streams?: MediaStream[];
        /** Validated in order: a rid that breaks RFC 8851, a duplicate rid, or some-but-not-all encodings with a rid is a
         *  TypeError; maxFramerate < 0 or scaleResolutionDownBy < 1 (video) is a RangeError. Then the list is cut to 1
         *  encoding (audio) or 4 (video); a single remaining encoding loses its rid. Two or more video encodings offer
         *  a=rid send lines and a=simulcast:send. */
        sendEncodings?: RTCRtpEncodingParameters[];
    }

    /** webrtc-pc 5.2. getParameters() returns a transactionId that stays the same within one task and is cleared by the
     *  next macrotask (or by setParameters); setParameters() rejects, in this order: TypeError for a missing
     *  transactionId, encodings, headerExtensions, rtcp or codecs; InvalidStateError when the transceiver is stopping or
     *  there was no getParameters() in this task; InvalidModificationError for another transactionId, a changed number
     *  of encodings, a changed rid, or changed codecs, headerExtensions or rtcp; RangeError for scaleResolutionDownBy < 1
     *  or maxFramerate < 0. Encoding values are stored and reported back, not yet applied to media (media flow is not
     *  part of this module). */
    class RTCRtpSender {
        readonly track: MediaStreamTrack | null;
        /** null until the transceiver is associated with an m= section (setLocalDescription / setRemoteDescription),
         *  then the connection's one RTCDtlsTransport (everything is bundled), the same object for every sender,
         *  receiver and sctp.transport. */
        readonly transport: RTCDtlsTransport | null;
        /** An RTCDTMFSender for an audio sender (the same object each time), null for video. */
        readonly dtmf: RTCDTMFSender | null;
        static getCapabilities(kind: string): RTCRtpCapabilities | null;
        getParameters(): RTCRtpSendParameters;
        setParameters(parameters: RTCRtpSendParameters): Promise<void>;
        /** Chained on the connection's operations queue. A track of another kind rejects with TypeError, a stopping
         *  transceiver with InvalidStateError; needs no renegotiation. A remote track (an RTCRtpReceiver's,
         *  from this or another connection) given to addTrack / addTransceiver / replaceTrack is relayed: every packet its receiver accepts
         *  is sent on this sender with this sender's SSRC, sequence and timestamp base, the payload type mapped by
         *  codec name, clock rate and channels to this m= section (packets with no match are skipped), CSRCs and the
         *  RFC 6464 / 6465 level bytes kept. The mapping follows renegotiation on either side. A tone from this
         *  sender's RTCDTMFSender takes the next sequence numbers and relaying continues after it (the tone keeps its
         *  own wall-clock RTP timestamp). Relaying stops on replaceTrack(null) or another track and when the relayed
         *  track ends. */
        replaceTrack(track: MediaStreamTrack | null): Promise<void>;
        /** Replaces the stream ids signalled in a=msid; fires negotiationneeded. Non-MediaStream arguments are a
         *  TypeError; a closed connection an InvalidStateError. */
        setStreams(...streams: MediaStream[]): void;
        /** Resolves an empty Map (RTP statistics come with media flow). */
        getStats(): Promise<Map<string, Record<string, unknown>>>;
    }

    class RTCRtpReceiver {
        /** Created with the transceiver; never null. */
        readonly track: MediaStreamTrack;
        /** Same object as RTCRtpSender.transport. */
        readonly transport: RTCDtlsTransport | null;
        /** ms, 0..4000 (RangeError outside), or null for the default. */
        jitterBufferTarget: number | null;
        static getCapabilities(kind: string): RTCRtpCapabilities | null;
        getParameters(): RTCRtpParameters;
        /** One entry per CSRC (getContributingSources) or SSRC (getSynchronizationSources) seen in the last 10 s,
         *  newest first (webrtc-pc 5.3). A packet counts once it passes SRTP authentication and BUNDLE routing (MID
         *  header extension, then a signalled or latched SSRC, then a payload type unique to one receiver, RFC 8843
         *  9.2); nothing is decoded before P3, so that is when it is "delivered to the track". An older RTP timestamp
         *  never overwrites a newer one. timestamp is wall-clock ms (comparable to Date.now(); dynajs has no
         *  performance.timeOrigin). audioLevel only for audio, from RFC 6464 (SSRC) / RFC 6465 (CSRC; offered as
         *  urn:ietf:params:rtp-hdrext:csrc-audio-level), 127 -> 0, else 10^(-level/20); absent without the value. Up to
         *  4 SSRCs and 16 CSRCs are kept per receiver (the oldest is replaced). The remote track starts muted, is
         *  unmuted (a task firing "unmute") by the first such packet while the transceiver is receptive (local
         *  direction includes recv), and muted again by an RTCP BYE for its SSRC or after 25 s without RTP or an RTCP sender report from
         *  its source (RFC 3550 6.3.5, 5 x the 5 s minimum report interval); the next packet unmutes it again.
         *  [[Receptive]] is set by setLocalDescription from the direction in the description's m= line (fixed when the
         *  offer or answer was created), not from transceiver.direction at that moment. */
        getContributingSources(): { timestamp: number; source: number; audioLevel?: number; rtpTimestamp: number }[];
        getSynchronizationSources(): { timestamp: number; source: number; audioLevel?: number; rtpTimestamp: number }[];
        getStats(): Promise<Map<string, Record<string, unknown>>>;
    }

    /** webrtc-pc 5.4. Setting direction to an unknown value is ignored (WebIDL enum attribute), to "stopped" is a
     *  TypeError, on a stopping transceiver an InvalidStateError; a change fires negotiationneeded. stop() makes it
     *  stopping (direction "stopped"; the receiver track is ended at once and its "ended" event fires in a later task,
     *  webrtc-pc 5.4 stop sending and receiving); the next negotiation rejects its m= section
     *  (port 0), after which currentDirection is "stopped" and the transceiver
     *  leaves getTransceivers() once the signaling state returns to stable. stop() on a closed connection is an
     *  InvalidStateError. A transceiver stopped by the remote side (its m= section rejected) ends its
     *  receiver track with an "ended" event. */
    class RTCRtpTransceiver {
        readonly mid: string | null;
        readonly sender: RTCRtpSender;
        readonly receiver: RTCRtpReceiver;
        direction: RTCRtpTransceiverDirection;
        readonly currentDirection: RTCRtpTransceiverDirection | null;
        stop(): void;
        /** Orders and filters the codecs of the next offer or answer for this m= section. Each entry must match one of
         *  RTCRtpReceiver.getCapabilities(kind).codecs (mimeType case-insensitive, clockRate, channels, sdpFmtpLine)
         *  else InvalidModificationError; a list of only rtx/red/ulpfec/flexfec/CN is an InvalidModificationError;
         *  listing video/rtx keeps the RTX of every listed codec; [] restores the default order. */
        setCodecPreferences(codecs: RTCRtpCodec[]): void;
    }

    /** webrtc-pc 5.6. The connection's one ICE transport (component "rtp"; everything is bundled). state is the
     *  connection's iceConnectionState, gatheringState its iceGatheringState; role is "unknown" before ICE starts, then
     *  "controlling" (offerer, or the full agent facing ice-lite) or "controlled" (RFC 8445 6.1.1). The candidate lists
     *  are RTCIceCandidates built from the gathered / received candidates; getSelectedCandidatePair() is null until a
     *  pair is selected and after close(). getLocalParameters / getRemoteParameters are the a=ice-ufrag / a=ice-pwd
     *  in use (null before they are known). Events: statechange, gatheringstatechange, selectedcandidatepairchange.
     *  close() sets state "closed" without an event. Not constructible (TypeError). */
    class RTCIceTransport extends EventTarget {
        readonly role: "unknown" | "controlling" | "controlled";
        readonly component: "rtp";
        readonly state: RTCIceConnectionState;
        readonly gatheringState: RTCIceGatheringState;
        getLocalCandidates(): RTCIceCandidate[];
        getRemoteCandidates(): RTCIceCandidate[];
        getSelectedCandidatePair(): { local: RTCIceCandidate; remote: RTCIceCandidate } | null;
        getLocalParameters(): { usernameFragment: string; password: string } | null;
        getRemoteParameters(): { usernameFragment: string; password: string } | null;
        onstatechange: ((ev: Event) => void) | null;
        ongatheringstatechange: ((ev: Event) => void) | null;
        onselectedcandidatepairchange: ((ev: Event) => void) | null;
    }

    /** webrtc-pc 5.5. The connection's one DTLS transport. state: "new" -> "connecting" -> "connected", or "failed"
     *  (an "error" event fires first with an RTCError, a DOMException OperationError whose errorDetail is
     *  "fingerprint-failure" when the peer certificate does not match the signalled a=fingerprint, else "dtls-failure"),
     *  or "closed" (close_notify received, or close() without an event). getRemoteCertificates() is [the peer's DER
     *  certificate as a new ArrayBuffer] while connected, else []. Not constructible (TypeError). */
    class RTCDtlsTransport extends EventTarget {
        readonly iceTransport: RTCIceTransport;
        readonly state: "new" | "connecting" | "connected" | "closed" | "failed";
        getRemoteCertificates(): ArrayBuffer[];
        onstatechange: ((ev: Event) => void) | null;
        onerror: ((ev: Event & { error: DOMException & { errorDetail: string } }) => void) | null;
    }

    /** webrtc-pc 7. canInsertDTMF is true only when the connection is "connected", the transceiver is not stopping, the
     *  sender has a track, currentDirection is sendrecv or sendonly, the first encoding is active and
     *  audio/telephone-event (RFC 4733) was negotiated. insertDTMF(tones, duration = 100, interToneGap = 70): when
     *  canInsertDTMF is false an InvalidStateError (checked before the characters); a character outside 0-9 A-D a-d # *
     *  ',' is an InvalidCharacterError; a-d become A-D; duration clamps to 40..6000 ms and interToneGap to 30..6000 ms;
     *  the call replaces toneBuffer and starts the playout task only if none is scheduled. Each task run removes one
     *  tone, schedules the next run after duration + interToneGap (2000 ms after ','), then fires tonechange with that
     *  tone; an empty buffer fires tonechange with tone "" and stops; once DTMF can no longer be sent the task stops
     *  without an event, leaving the rest in toneBuffer. Each tone (not ',') goes on the wire as RFC 4733 telephone-event
     *  packets on the negotiated telephone-event payload type whose clock matches the audio codec (else the first
     *  one): event 0-9 * # A-D = 0-15, volume 10, one RTP timestamp per tone, marker on the first packet, one update
     *  every 50 ms carrying the elapsed duration plus 50 ms (through the next update, never past the tone), then three
     *  end packets (E bit) with the full duration; a tone longer than 65535 clock units is split into segments (new
     *  timestamp, no marker).
     *  Not constructible (TypeError). */
    class RTCDTMFSender extends EventTarget {
        insertDTMF(tones: string, duration?: number, interToneGap?: number): void;
        readonly canInsertDTMF: boolean;
        readonly toneBuffer: string;
        ontonechange: ((ev: Event & { tone: string }) => void) | null;
    }

    interface RTCTrackEvent extends Event {
        readonly receiver: RTCRtpReceiver;
        readonly track: MediaStreamTrack;
        readonly streams: MediaStream[];
        readonly transceiver: RTCRtpTransceiver;
    }

    type RTCIceConnectionState = "new" | "checking" | "connected" | "completed" | "disconnected" | "failed" | "closed";
    type RTCPeerConnectionState = "new" | "connecting" | "connected" | "disconnected" | "failed" | "closed";
    type RTCDataChannelState = "connecting" | "open" | "closing" | "closed";
    type RTCIceCandidateType = "host" | "srflx" | "prflx" | "relay";

    /** One STUN or TURN server. urls: "stun:host[:port]" or "turn:host[:port][?transport=udp|tcp]"; validated by the W3C
     *  "validate an ICE server URL" steps (SyntaxError DOMException for a malformed URL or empty list, NotSupportedError for
     *  stuns:/turns:, InvalidAccessError for a turn: URL without username and credential). A turn: URL with
     *  ?transport=tcp is accepted and not used for gathering (TURN is used over UDP only). Host names resolve once at
     *  construction; a server no local socket can reach fires icecandidateerror 701 when gathering starts. */
    interface RTCIceServer {
        urls: string | string[];
        username?: string;
        credential?: string;
    }

    interface RTCConfiguration {
        iceServers?: RTCIceServer[];
        /** "relay" gathers relay candidates only (host and srflx are never offered nor used). */
        iceTransportPolicy?: "all" | "relay";
        /** Any value works the same here (the single data m-section is always bundled); getConfiguration reports it
         *  (default "balanced") and setConfiguration cannot change it. */
        bundlePolicy?: "balanced" | "max-compat" | "max-bundle";
        /** Only "require" is accepted (rtcp-mux is mandatory; "negotiate" is a NotSupportedError per webrtc-pc). */
        rtcpMuxPolicy?: "require";
        /** Certificates from RTCPeerConnection.generateCertificate(); default: a fresh ECDSA P-256 certificate. An expired
         *  one is an InvalidAccessError. The default one is generated at the first createOffer, createAnswer or implicit setLocalDescription() on the CPU
         *  offload pool, inside the operations chain: that operation and the ones queued after it wait for it without
         *  blocking the event loop, and a close() meanwhile leaves them unsettled; under --io-threads 0 it is generated
         *  inline. A keygen still running at exit is dropped by the shutdown sweep. */
        certificates?: RTCCertificate[];
        iceCandidatePoolSize?: number;
        /** dynajs extension: the IP literals (at most 8) to gather host candidates on, instead of every up interface.
         *  The default gathers every up interface except loopback, link-local, IPv6 site-local (fec0::/10), IPv4-compatible
         *  and IPv4-mapped IPv6 (RFC 8445 5.1.1.1); with none left there are no host candidates. Loopback is used only when
         *  listed here (tests). Not a webrtc-pc member; browsers ignore it. */
        iceLocalAddresses?: string[];
        /** dynajs extension for tests and constrained peers: the SCTP inbound/outbound stream counts advertised in
         *  INIT/INIT_ACK (1..65535). The default is 65535, per RFC 8831 6.2. maxChannels is the minimum of the negotiated
         *  counts in both directions. Not a webrtc-pc member; browsers ignore it. */
        sctpStreams?: number;
        /** dynajs extension for tests: false stops advertising RFC 9653 zero checksum (default true). */
        sctpZeroChecksum?: boolean;
    }

    interface RTCSessionDescriptionInit {
        type: RTCSdpType;
        sdp?: string;
    }

    interface RTCIceCandidateInit {
        candidate?: string;
        sdpMid?: string | null;
        sdpMLineIndex?: number | null;
        usernameFragment?: string | null;
    }

    interface RTCDataChannelInit {
        ordered?: boolean;
        /** At most one of maxPacketLifeTime (ms) and maxRetransmits (TypeError when both). Values clamp to 65535. */
        maxPacketLifeTime?: number;
        maxRetransmits?: number;
        /** At most 65535 UTF-8 bytes, as is label. */
        protocol?: string;
        /** true: no DCEP open; both sides create the channel with the same id (required then, 0..65534). */
        negotiated?: boolean;
        id?: number;
        priority?: "very-low" | "low" | "medium" | "high";
    }

    interface RTCOfferOptions {
        iceRestart?: boolean;
    }

    /** RTCSctpTransport as a plain object. maxMessageSize = min(local limit, remote a=max-message-size, where 0 means no
     *  limit); the local limit is 16 MiB and is what this side advertises. maxChannels is null until connected. */
    interface RTCSctpTransportInfo {
        /** The connection's RTCDtlsTransport. */
        readonly transport: RTCDtlsTransport;
        readonly state: "connecting" | "connected" | "closed";
        readonly maxMessageSize: number;
        readonly maxChannels: number | null;
    }

    interface RTCDataChannelEvent extends Event {
        readonly channel: RTCDataChannel;
    }
    interface RTCPeerConnectionIceEvent extends Event {
        /** null marks the end of gathering for this generation. */
        readonly candidate: RTCIceCandidate | null;
        readonly url: string | null;
    }
    /** errorCode: the STUN error code from the server (401, 437, ...), or 701 when the server could not be reached
     *  (RFC 8489 6.2.1 retransmission schedule exhausted: 7 requests, 39.5 s). */
    interface RTCPeerConnectionIceErrorEvent extends Event {
        readonly address: string | null;
        readonly port: number | null;
        readonly url: string;
        readonly errorCode: number;
        readonly errorText: string;
    }
    interface MessageEvent extends Event {
        /** string for a text message; ArrayBuffer or Blob (by binaryType) for a binary one. */
        readonly data: string | ArrayBuffer | Blob;
    }

    class RTCSessionDescription {
        constructor(init: RTCSessionDescriptionInit);
        readonly type: RTCSdpType;
        readonly sdp: string;
        toJSON(): RTCSessionDescriptionInit;
    }

    /** Parses the candidate attribute (RFC 8839 5.1) at construction; a malformed one is a TypeError only when sdpMid and
     *  sdpMLineIndex are both null. Getters expose the parsed fields (null when absent). */
    class RTCIceCandidate {
        constructor(init?: RTCIceCandidateInit);
        readonly candidate: string;
        readonly sdpMid: string | null;
        readonly sdpMLineIndex: number | null;
        readonly foundation: string | null;
        readonly component: "rtp" | "rtcp" | null;
        readonly priority: number | null;
        readonly address: string | null;
        readonly protocol: "udp" | "tcp" | null;
        readonly port: number | null;
        readonly type: RTCIceCandidateType | null;
        readonly tcpType: string | null;
        readonly relatedAddress: string | null;
        readonly relatedPort: number | null;
        readonly usernameFragment: string | null;
        readonly relayProtocol: "udp" | null;
        readonly url: string | null;
        toJSON(): RTCIceCandidateInit;
    }

    class RTCCertificate {
        /** ms since the epoch. */
        readonly expires: number;
        getFingerprints(): { algorithm: "sha-256"; value: string }[];
    }

    /** A data channel. send() accepts string (sent as UTF-8, PPID 51, or 56 when empty), ArrayBuffer, any ArrayBufferView
     *  or Blob (PPID 53, or 57 when empty); it throws InvalidStateError unless readyState is "open" and TypeError when the
     *  byte length exceeds pc.sctp.maxMessageSize. Messages larger than one SCTP chunk are fragmented and reassembled
     *  (RFC 9260 6.9). bufferedAmount counts user bytes queued and not yet acknowledged, never DCEP or padding bytes. close()
     *  resets the stream (RFC 8831 6.7): "closing", then "close" once both directions are reset; the channel keeps its JS
     *  object alive until then. */
    class RTCDataChannel extends EventTarget {
        readonly label: string;
        readonly ordered: boolean;
        readonly maxPacketLifeTime: number | null;
        readonly maxRetransmits: number | null;
        readonly protocol: string;
        readonly negotiated: boolean;
        readonly id: number | null;
        readonly readyState: RTCDataChannelState;
        readonly bufferedAmount: number;
        bufferedAmountLowThreshold: number;
        /** "blob" (default, per webrtc-pc) or "arraybuffer". */
        binaryType: "blob" | "arraybuffer";
        readonly priority: "very-low" | "low" | "medium" | "high";
        onopen: ((ev: Event) => void) | null;
        onmessage: ((ev: MessageEvent) => void) | null;
        onbufferedamountlow: ((ev: Event) => void) | null;
        onerror: ((ev: Event) => void) | null;
        onclosing: ((ev: Event) => void) | null;
        onclose: ((ev: Event) => void) | null;
        send(data: string | ArrayBuffer | ArrayBufferView | Blob): void;
        close(): void;
    }

    /** One peer connection. Operations (createOffer, createAnswer, setLocalDescription, setRemoteDescription,
     *  addIceCandidate) run one at a time in call order (the webrtc-pc operations chain) and reject with
     *  InvalidStateError once closed. The offerer is ICE-controlling (unless the answer is ice-lite) and the DTLS server
     *  (a=setup:actpass; the answer says active), so its data-channel ids are odd and the answerer's even (RFC 8832 6).
     *  Trickle ICE: candidates arrive as icecandidate events after setLocalDescription settles; localDescription also
     *  lists every candidate gathered so far, so a non-trickle peer can wait for iceGatheringState "complete".
     *  getStats() resolves an RTCStatsReport (a Map) with peer-connection, data-channel, transport, sctp-transport,
     *  local-candidate, remote-candidate, candidate-pair and certificate entries, also after close(). The sctp-transport
     *  entry adds dynajs fields forwardTsnSent, initChunksSent, zeroChecksumNegotiated, zeroChecksumPacketsSent and
     *  zeroChecksumPacketsReceived; the transport entry adds receiveWakeups (event drains, one per socket wakeup that
     *  delivered datagrams, so it stays below packetsReceived under bursts). SCTP zero checksum (RFC 9653, EDMID 1 = SCTP over DTLS) is advertised in INIT and
     *  INIT ACK; packets go out with a zero checksum only after the peer advertised it and only in established/shutdown
     *  states (INIT, INIT ACK and COOKIE ECHO always carry CRC32c); a zero checksum is accepted only when we advertised
     *  it, never on INIT or COOKIE ECHO, never before our INIT/INIT ACK went out; a correct CRC32c is always accepted.
     *  Threading: one RTCPeerConnection, its channels, ICE, DTLS and SCTP state live on the event loop of the realm that
     *  created it; the packet path (UDP receive, STUN, DTLS records, SCTP, DCEP, events) runs on that loop thread with no
     *  locks, and neither the connection nor a channel can be posted to another Worker (postMessage
     *  throws). generateCertificate generates the key on the shared io pool and settles on the loop; with
     *  --io-threads 0 it runs inline on the loop thread.
     *  SCTP: both endpoints send INIT once DTLS connects (RFC 8841 10.2 lets either side start; a simultaneous open is
     *  the RFC 9260 5.2.1 INIT collision). An idle association sends HEARTBEAT every RTO + 30 s (RFC 9260 8.3); each
     *  unanswered one doubles RTO and counts toward Association.Max.Retrans (10), after which the association aborts
     *  (about 12 minutes for an idle peer that vanished; the SCTP layer test measures it), so a silent dead peer is
     *  detected even when nothing is sent; an answered HEARTBEAT clears the error count. */
    class RTCPeerConnection extends EventTarget {
        /** dynaOptions is a dyna extension (see RTCPeerConnectionDynaOptions), validated after the configuration; an invalid one throws and no connection is made. */
        constructor(configuration?: RTCConfiguration, dynaOptions?: RTCPeerConnectionDynaOptions);
        /** keygenAlgorithm: { name: "ECDSA", namedCurve: "P-256" } or { name: "RSASSA-PKCS1-v1_5", modulusLength
         *  1024..8192, publicExponent 65537, hash: "SHA-256" }, optionally with expires (ms, capped at 365 days). Anything
         *  else rejects with NotSupportedError. */
        static generateCertificate(keygenAlgorithm: string | object): Promise<RTCCertificate>;
        createOffer(options?: RTCOfferOptions): Promise<RTCSessionDescriptionInit>;
        createAnswer(): Promise<RTCSessionDescriptionInit>;
        /** With no argument: the implicit offer or answer for the current signaling state. */
        setLocalDescription(description?: RTCSessionDescriptionInit): Promise<void>;
        /** Validates the whole description before changing anything, so a rejected one leaves every state as it was
         *  (InvalidAccessError for a missing ice-ufrag/ice-pwd, fingerprint, an answer that does not match the offer or
         *  says actpass, or an answer that changes the ICE credentials of an offer that did not restart ICE;
         *  OperationError with errorDetail "sdp-syntax-error" for unparsable SDP); a valid offer in have-local-offer
         *  rolls the local offer back first. An ICE restart (either side) takes effect when its answer is applied: until
         *  then the current credentials, candidates and selected pair stay in use, and a rollback (explicit or by glare)
         *  drops it, after which the next offer restarts again if restartIce() asked for it. */
        setRemoteDescription(description: RTCSessionDescriptionInit): Promise<void>;
        /** null or { candidate: "" } marks end-of-candidates. */
        addIceCandidate(candidate?: RTCIceCandidateInit | null): Promise<void>;
        getStats(): Promise<Map<string, Record<string, unknown>>>;
        createDataChannel(label: string, init?: RTCDataChannelInit): RTCDataChannel;
        /** Closes every channel and the transports at once (DTLS close_notify, TURN allocations refreshed to lifetime 0);
         *  fires no state events (webrtc-pc). Operations queued before close() (createOffer, setRemoteDescription, ...)
         *  never settle, nor does one whose own events (signalingstatechange) called close(); calls made after it reject
         *  with InvalidStateError, except getStats. */
        close(): void;
        /** The next offer carries fresh ICE credentials; fires negotiationneeded. The ICE role is set by the initial
         *  offer/answer and an ICE restart never redetermines it (RFC 8445 6.1.1); only a peer turning ICE-lite or a role
         *  conflict (487) changes it. */
        restartIce(): void;
        getConfiguration(): RTCConfiguration;
        /** Changing certificates or bundlePolicy (an omitted one means "balanced") is an InvalidModificationError; an
         *  unknown bundlePolicy or rtcpMuxPolicy is a TypeError; an invalid configuration throws and changes nothing.
         *  getConfiguration reports the new iceTransportPolicy at once, but gathering, signalling and pairing follow it
         *  only from the next gathering.
         *  iceServers (omitted = none), iceTransportPolicy (omitted = "all") and iceLocalAddresses (omitted = kept) apply
         *  from the next gathering, the ICE restart: it opens sockets on interfaces that appeared, starts STUN/TURN for
         *  the new servers, and under "relay" signals and pairs only relay candidates; candidates already gathered and
         *  the selected pair stay in use until the restart selects a new pair. A server listed again with the same url,
         *  username and credential keeps its allocation (no second Allocate); switching "relay" back to "all" signals
         *  the host and srflx candidates of the existing sockets again; under "relay" the local description lists
         *  only relay candidates. A server no longer listed stops being signalled
         *  and paired at the restart, keeps carrying the selected pair, and is released (TURN Refresh LIFETIME 0)
         *  when the restart selects a pair on another session, so a relayed connection never loses its path while
         *  moving between servers; a pair the peer selects on it before then (renomination) keeps it alive until the
         *  next selection. A connection holds at most 64 live STUN/TURN sessions (servers x sockets); a released
         *  session frees its slot for the next one, sessions not yet released still count, and a server that finds
         *  no free slot gets an icecandidateerror with errorCode 701 and errorText "STUN/TURN session limit (64)
         *  reached" instead of being skipped silently. */
        setConfiguration(configuration: RTCConfiguration): void;
        /** Senders and receivers of transceivers that are not stopped; transceivers in creation order, stopped ones until
         *  they are removed (webrtc-pc 4.4.1.6). close() stops every transceiver without events and keeps them listed. */
        getSenders(): RTCRtpSender[];
        getReceivers(): RTCRtpReceiver[];
        getTransceivers(): RTCRtpTransceiver[];
        /** Reuses a transceiver of the same kind that never sent, has no track and is not stopping (recvonly becomes
         *  sendrecv, inactive becomes sendonly); otherwise creates a sendrecv one. TypeError for a non-track or non-stream
         *  argument, InvalidAccessError when the track already has a sender here, InvalidStateError when closed. */
        addTrack(track: MediaStreamTrack, ...streams: MediaStream[]): RTCRtpSender;
        /** sendrecv becomes recvonly, sendonly becomes inactive; a sender of another connection is an InvalidAccessError. */
        removeTrack(sender: RTCRtpSender): void;
        addTransceiver(trackOrKind: MediaStreamTrack | "audio" | "video", init?: RTCRtpTransceiverInit): RTCRtpTransceiver;
        readonly localDescription: RTCSessionDescription | null;
        readonly currentLocalDescription: RTCSessionDescription | null;
        readonly pendingLocalDescription: RTCSessionDescription | null;
        readonly remoteDescription: RTCSessionDescription | null;
        readonly currentRemoteDescription: RTCSessionDescription | null;
        readonly pendingRemoteDescription: RTCSessionDescription | null;
        readonly signalingState: RTCSignalingState;
        readonly iceGatheringState: RTCIceGatheringState;
        readonly iceConnectionState: RTCIceConnectionState;
        readonly connectionState: RTCPeerConnectionState;
        readonly canTrickleIceCandidates: boolean | null;
        /** null until a description with an application m-section is applied. */
        readonly sctp: RTCSctpTransportInfo | null;
        onicecandidate: ((ev: RTCPeerConnectionIceEvent) => void) | null;
        onicecandidateerror: ((ev: RTCPeerConnectionIceErrorEvent) => void) | null;
        onsignalingstatechange: ((ev: Event) => void) | null;
        oniceconnectionstatechange: ((ev: Event) => void) | null;
        onicegatheringstatechange: ((ev: Event) => void) | null;
        onconnectionstatechange: ((ev: Event) => void) | null;
        ondatachannel: ((ev: RTCDataChannelEvent) => void) | null;
        onnegotiationneeded: ((ev: Event) => void) | null;
        /** Fires inside setRemoteDescription, before its promise resolves (webrtc-pc 4.4.1.6: mute, then removetrack,
         *  then addtrack, then track events), once per transceiver that starts receiving: an offer associates its
         *  m-sections by mid, then with an unassociated addTrack transceiver of the same kind, else creates a recvonly
         *  one. A rollback removes the transceivers the remote offer created. */
        ontrack: ((ev: RTCTrackEvent) => void) | null;
    }
}

/** dyna:wireguard — WireGuard (Noise_IKpsk2, ChaCha20-Poly1305) over UDP, with a userspace IPv4/IPv6 + TCP (SACK, RFC 6528 ISN) stack or a kernel tun.
  *  X25519, AEAD batches and inner checksums run on the pool (a lone packet <=256 B is sealed inline: cheaper than a handoff). */
declare module "dyna:wireguard" {
    /** One peer; keys are base64 of 32 bytes. Unknown keys throw/reject TypeError naming the key. */
    interface WireGuardPeer {
        publicKey: string;
        presharedKey?: string;
        /** "a.b.c.d:port", an IPv4 literal (resolve names first; an IPv6 underlay is a TypeError); default none (learned from the first packet). */
        endpoint?: string;
        /** CIDRs routed to this peer, e.g. "10.0.0.2/32"; at least one. */
        allowedIPs: string[];
        /** Seconds between keepalives, 0..65535; default 0 (off). */
        persistentKeepalive?: number;
    }
    /** WireGuard.create options; unknown keys reject TypeError naming the key. */
    interface WireGuardOptions {
        /** Base64 of 32 bytes (X25519 private key; clamped as in RFC 7748). */
        privateKey: string;
        /** UDP port on 0.0.0.0, 0..65535; default 0 (ephemeral, see listenPort). */
        listenPort?: number;
        /** Tunnel addresses of this side ("10.0.0.1/24" or "fd00::1"); the prefix is ignored (routing is by allowedIPs). Required without tun. */
        address?: string[];
        peers?: WireGuardPeer[];
        /** Kernel tun interface name (utun<N> on macOS, any name on Linux); needs root or CAP_NET_ADMIN; dial/listen/udp then throw TypeError. */
        tun?: string;
        /** Tun MTU, 1280..65535; default 1420. */
        mtu?: number;
    }
    /** Byte stream over one tunnelled TCP connection. */
    interface WireGuardConn {
        /** Received bytes; a slow reader closes the TCP window (backpressure). cancel() aborts the connection. */
        readonly readable: ReadableStream<Uint8Array>;
        /** A write resolves once the stack accepted all its bytes; close() sends FIN after the queue drains; abort() sends RST. */
        readonly writable: WritableStream<ArrayBufferView | ArrayBuffer | string>;
        readonly remoteAddress: string;
        readonly remotePort: number;
        readonly localAddress: string;
        readonly localPort: number;
        /** RST now; the readable closes and the writable errors. Idempotent. */
        close(): void;
    }
    interface WireGuardListener {
        readonly port: number;
        /** Stops accepting; open connections stay. Idempotent. */
        close(): void;
    }
    /** UDP socket inside the tunnel (both families when both are configured). */
    interface WireGuardUdp {
        readonly port: number;
        /** Datagram of at most 65507 bytes to an IP literal; throws TypeError on a bad address or a closed socket, false when not routable. */
        send(data: ArrayBufferView | ArrayBuffer | string, host: string, port: number): boolean;
        close(): void;
    }
    interface WireGuardStats {
        peers: number;
        handshakesSent: number;
        handshakesCompleted: number;
        rxPackets: number;
        txPackets: number;
        rxDropped: number;
        txDropped: number;
        /** Inner packets whose L4 checksum the pool verified, so the stack skipped it. */
        checksumsVerified: number;
        poolJobs: number;
        /** Lone packets <=256 B (ACKs, keepalives) sealed or opened on the JS thread, plus any job the pool refused. */
        inlineJobs: number;
        tunRx: number;
        tunTx: number;
        tcpConnections: number;
        timerFires: number;
    }
    /** A WireGuard device on the shared reactor; it keeps the process alive until close(). */
    class WireGuard implements DynResource {
        /** Not constructible: TypeError; use WireGuard.create. */
        private constructor();
        /** Resolves once the UDP socket is bound and every peer's static key is derived (pool). Rejects TypeError on bad options, a bad
         *  key or a tun open failure ("WireGuard: tun: <stage>: <errno text>"); Error on a low-order peer key or a bind failure. */
        static create(opts: WireGuardOptions): Promise<WireGuard>;
        /** wg-quick INI ([Interface], [Peer]); keys are case-insensitive; DNS, Table, FwMark, PreUp/PostUp/PreDown/PostDown and SaveConfig are ignored.
         *  Throws SyntaxError "WireGuard.parseConfig: line N: ..." on an unknown key, a duplicate key or a bad number. */
        static parseConfig(text: string): WireGuardOptions;
        /** A fresh X25519 key pair (base64), generated on the pool. */
        static generateKeyPair(): Promise<{ privateKey: string; publicKey: string }>;
        readonly publicKey: string;
        readonly listenPort: number;
        /** Tun interface name, or null with the userspace stack. */
        readonly tun: string | null;
        /** TCP connect to an IP literal inside the tunnel; rejects Error "WireGuard: connect: <reason>" (no route to host, refused, timed out, closed). */
        dial(host: string, port: number): Promise<WireGuardConn>;
        /** Accepts on port (1..65535) on every configured family; handler runs once per connection. TypeError when the port is taken. */
        listen(port: number, handler: (conn: WireGuardConn) => void): WireGuardListener;
        /** Binds port (0 = ephemeral); handler gets each datagram. TypeError when the port is taken. */
        udp(port: number, handler: (data: Uint8Array, host: string, port: number) => void): WireGuardUdp;
        /** Resolves when the peer is usable; rejects TypeError on a bad or duplicate key, Error on a low-order key or removePeer/close first. */
        addPeer(peer: WireGuardPeer): Promise<void>;
        /** Drops the peer, its sessions and routes; false when unknown. */
        removePeer(publicKey: string): boolean;
        stats(): WireGuardStats;
        /** Closes every connection, socket and the device; pending promises reject Error "WireGuard: closed". Idempotent. */
        close(): void;
        dispose(): void;
        readonly closed: boolean;
        readonly [Symbol.dispose]: () => void;
    }
}

/** dyna:vec — approximate and exact nearest-neighbour search over f32 vectors (HNSW and Flat), with 64-bit ids, removal and id filters. */
declare module "dyna:vec" {
    /** Distance metric. l2 = squared Euclidean distance; cosine = 1 - cos(a, b), computed on vectors scaled to unit length and rounded to f32 when stored; ip = -dot(a, b), so that a smaller distance always ranks first. */
    type VectorMetric = "l2" | "cosine" | "ip";

    /** Index kind. flat = scan of every vector (exact with f32; with sq8/binary see VectorQuantization); hnsw = hierarchical navigable small world graph (approximate); "ivfpq" is rejected with RangeError (unsupported in this build). */
    type VectorIndexKind = "hnsw" | "ivfpq" | "flat";

    /**
     * Stored vector format. "f32" stores 32-bit floats and every distance is computed from them.
     *
     * "sq8" stores each component as a signed byte code c in [-127, 127]: c = clamp(floor((x - mid_j) / s + 0.5), -127, 127),
     * where mid_j = (lo_j + hi_j) / 2 for each dimension j and one step s = max_j (hi_j - lo_j) / 254 is shared by all
     * dimensions (s = 0 gives code 0). [lo_j, hi_j] covers the stored components (for cosine, after scaling to unit
     * length): the first add sets it to that batch's per-dimension minimum and maximum. A later add with a component
     * outside the range widens each such dimension to cover the batch, plus 25% of the widened width on every side that
     * grew, and re-encodes every stored vector: from its f32 copy, or with dyna.rerank false from its decoded codes
     * (mid_j + s * c_j), which adds rounding. The query is encoded the same way and clamped to the range. Candidate
     * distances are integer sums over the codes a (query) and b (stored): l2 = s^2 * sum_j (a_j - b_j)^2;
     * dot = sum_j mid_j^2 + s * (sum_j mid_j a_j + sum_j mid_j b_j) + s^2 * sum_j a_j b_j, so ip = -dot and cosine = 1 - dot.
     *
     * "binary" stores one bit per dimension, set when the stored component is greater than 0, packed little-endian into
     * 64-bit words (bits past dims are 0). The candidate distance is the Hamming distance (popcount of the XOR) for
     * every metric, so recall depends on data centred on zero, as embedding models produce; it is lower than sq8.
     *
     * With sq8 or binary, the candidate stage ranks by the candidate distance: the hnsw beam, the flat scan, and the
     * filtered exact fallback. With dyna.rerank true (the default), the candidate pool P = min(max(efSearch, k) * dyna.rerankFactor,
     * size) is widened before reranking: the hnsw beam and the flat scan keep the best P by the candidate distance (for
     * size, tombstones count), and those P are re-scored with the stored f32 copies, and the k nearest by that distance are returned with the
     * same distances an f32 index computes. With dyna.rerank false, no f32 copy is kept and the returned distances are
     * candidate distances (sq8: the formula above; binary: bit counts). The hnsw graph is built with f32 distances when
     * the f32 copy is kept, and otherwise with candidate distances between stored codes.
     *
     * Bytes per vector, before ids and links: f32 4*dims. sq8 adds dims, plus 8 for ip and cosine (the per-vector
     * sum_j mid_j b_j). binary adds 8*ceil(dims/64). With rerank false, only the codes are kept.
     */
    type VectorQuantization = "f32" | "sq8" | "binary";

    /**
     * dyna-specific meta configuration (not part of the portable VectorIndex shape). Every feature is on by default;
     * these keys only tune or switch off costly ones. hnsw only: any key here throws TypeError with index "flat", except
     * efSearch, rerank and rerankFactor, which a flat index with sq8 or binary accepts. Unknown keys throw TypeError.
     */
    interface VectorIndexMetaOptions {
        /** Default beam width for search, an integer in [1, 65536]; a search's efSearch overrides it and the beam used is max(efSearch, k). Default 128 (at 64, recall@10 on clustered cosine data fell to 0.925). */
        efSearch?: number;
        /**
         * Default true. When a search has a filter and its cardinality c (counted over the filter, including ids absent
         * from the index) satisfies c <= P (the candidate pool; max(efSearch, k) without a rerank; see VectorQuantization) or c * 16 <= size, the search scans the filter's ids instead of
         * the beam (exact with f32; with sq8 or binary it ranks by the candidate distance and reranks). false always uses the filtered beam, which then walks most of the graph for a
         * selective filter and is no cheaper; it exists only to measure the beam.
         */
        exactFallback?: boolean;
        /** Default true: neighbours are chosen with the HNSW diversity heuristic; for ip the heuristic compares Euclidean distances between the stored vectors, because -dot is not a metric and the plain rule leaves low-norm vectors with no in-links (unreachable by search and filters). false keeps the closest m (cheaper builds, much lower recall on clustered data). */
        heuristic?: boolean;
        /** Default true. sq8 and binary only (TypeError with f32): keep an f32 copy of every vector and re-score the candidates with it; see VectorQuantization. false keeps only the codes (4x smaller for sq8, 32x for binary) and returns candidate distances. */
        rerank?: boolean;
        /**
         * dyna extension, sq8 and binary with rerank true only (TypeError with f32 or rerank false): an integer in [1, 64]
         * that multiplies max(efSearch, k) into the candidate pool that is reranked (see VectorQuantization); saved with
         * the index. Default 12 for binary and 1 for sq8. 64 bits per vector order neighbours coarsely, so binary reranks
         * 12x the beam: on 4000 vectors of 64 dims, recall@10 rose from 0.45-0.74 (factor 1) to 0.958-0.993 (factor 12;
         * factor 8 gave 0.905 on clustered l2). The pool is fixed by k and efSearch, not by size, so recall at the same
         * factor falls as the index grows; raise it (or efSearch) for large indexes, lower it for speed.
         */
        rerankFactor?: number;
        /** Seed for the layer draw, an integer in [0, 2^53-1]. Default 0. The same seed, options and call sequence build the same graph and return the same results. */
        seed?: number;
    }

    /** Constructor options; unknown keys throw TypeError. m and efConstruction throw TypeError with index "flat". */
    interface VectorIndexOptions {
        /** Vector length, an integer in [1, 65536]; required. */
        dims: number;
        /** Default "l2". */
        metric?: VectorMetric;
        /** Default "hnsw". */
        index?: VectorIndexKind;
        /** Default "f32". */
        quantization?: VectorQuantization;
        /** Links per node on the upper layers, an integer in [2, 256]; layer 0 keeps 2*m. Default 16. */
        m?: number;
        /** Beam width while inserting, an integer in [1, 65536]. Default 200. */
        efConstruction?: number;
        /** dyna-specific meta configuration; see VectorIndexMetaOptions. */
        dyna?: VectorIndexMetaOptions;
    }

    /** Search options; unknown keys throw TypeError. */
    interface VectorSearchOptions {
        /** Beam width for this call, an integer in [1, 65536]; the beam used is max(efSearch, k). With sq8 or binary and dyna.rerank true, the beam and the flat scan keep max(efSearch, k) * dyna.rerankFactor candidates (at most size) for the rerank; with rerank false, max(efSearch, k); a flat f32 index ignores it. */
        efSearch?: number;
        /**
         * Allowed ids. Only ids in the filter are returned; ids in the filter that are not in the index are ignored. The
         * beam still walks through disallowed and removed nodes. See dyna.exactFallback.
         * A RoaringBitmap or BitSet holds uint32, so index ids at or above 2^32 never pass one. For 64-bit ids, pass a
         * BigUint64Array sorted strictly ascending (no duplicates); anything else rejects with RangeError. It is a plain
         * typed array, not a new class, because ids already travel as BigUint64Array, a sorted array tests membership by
         * binary search with no build step, and its length is the cardinality the exact fallback needs. The array is
         * copied when search is called. The BigUint64Array and BitSet forms are dyna-specific extensions.
         */
        filter?: import("dyna:structures").RoaringBitmap | import("dyna:structures").BitSet | BigUint64Array;
    }

    /** Nearest neighbours of one query, nearest first; equal distances (compared in double precision) rank by the smaller id. Length is min(k, matching vectors). */
    interface VectorSearchResult {
        ids: BigUint64Array;
        /** Distances computed in double precision over the stored f32 components, per the metric, then rounded to f32; with sq8 or binary and dyna.rerank false, the candidate distances (see VectorQuantization). */
        distances: Float32Array;
    }

    /** dyna-specific. Nearest neighbours of many queries, k slots per query in query order. */
    interface VectorBatchResult {
        /** Row q holds slots [q*k, q*k+k); slots past counts[q] hold 18446744073709551615n. */
        ids: BigUint64Array;
        /** Slots past counts[q] hold Infinity. */
        distances: Float32Array;
        /** Number of results found for each query. */
        counts: Uint32Array;
    }

    /**
     * An in-memory vector index of f32 vectors, stored as f32, sq8 or binary codes (see VectorQuantization), keyed by 64-bit ids, integers in [0, 2^64-2] (2^64-1 is reserved as the
     * padding id); ids arrive as BigUint64Array or Uint32Array and are mapped to dense internal slots. Vectors are
     * Float32Array, dims floats each, copied in; NaN, Infinity, and for cosine a zero vector, are refused.
     *
     * add, search and searchBatch return Promises and run one at a time, in call order: a call made while earlier ones
     * are unsettled waits for them (so a search sees every add called before it). A call with no earlier unsettled
     * call and little work runs to completion before it returns its Promise; large ones run off the event loop.
     * Arguments are validated and copied when the method is called, and argument errors reject the returned Promise.
     * remove and has are synchronous and throw TypeError while any add or search is unsettled; close throws TypeError
     * then too (DynResource). Any use after close throws or rejects with TypeError.
     *
     * remove leaves a tombstone: the vector is never returned (with or without a filter, and in the exact fallback)
     * but keeps its memory and stays in the graph as a waypoint. A removed id may be added again; it gets a new slot.
     *
     * save(path, opts?) writes one dyna persist v1 file and VectorIndex.load(path, opts?) reads it back; both are
     * synchronous. File format, atomic rename, fsync and the Error .code values are those of dyna:structures (see its
     * FILES notes); load refuses a file of another persist type with EPERSIST_TYPE. The file holds the options, the
     * layer-draw state, each slot's vector (its f32 copy when kept, its sq8 or binary codes, and the sq8 range), id and
     * tombstone flag (the id map is rebuilt from ids and tombstones on load) and, for hnsw, each node's level and links, so a loaded index returns exactly what the saved one returned
     * and later adds build the same graph. The same index state always saves to the same bytes (fixed lineage,
     * generation 1), so save, load, save reproduces the file. incremental is accepted and always writes a full save.
     * mmap: true maps the file privately and searches the stored f32 vectors in place (codes are copied), copy-on-write, never modifying the
     * file; the first add that grows the index copies them out. Before trusting a file, load checks every size and
     * range, that tombstone flags are 0 or 1 and match the live count, that live ids are unique and not 2^64-1, that
     * every component is finite, that the quantization matches its sections and flags, that the rerank factor is in [1, 64] when an f32 copy is reranked and 0 otherwise, that the sq8 range is finite and
     * ordered and its codes are in [-127, 127], that binary codes set no bit past dims, and for hnsw that the entry point is a top-layer node, that each link list is within
     * its degree bound and that every link targets another existing node on that layer; a failed check throws
     * EPERSIST_CORRUPT.
     */
    class VectorIndex implements DynResource {
        /** @throws {TypeError} when opts is not an object, dims is missing, a key is unknown, an option has the wrong type, or an hnsw-only option is given with index "flat", or dyna.rerank with quantization "f32", or dyna.rerankFactor with quantization "f32" or dyna.rerank false. @throws {RangeError} when an option is out of range, or index "ivfpq" (unsupported in this build). */
        constructor(opts: VectorIndexOptions);
        close(): void;
        dispose(): void;
        readonly closed: boolean;
        readonly [Symbol.dispose]: () => void;
        /** dyna-specific. Number of vectors (removed ones excluded); while an add is unsettled it holds the size before that add. */
        readonly size: number;
        /** dyna-specific. */
        readonly dims: number;
        /** dyna-specific. */
        readonly metric: VectorMetric;
        /** dyna-specific. */
        readonly index: VectorIndexKind;
        /** dyna-specific. */
        readonly quantization: VectorQuantization;
        /**
         * Inserts ids[i] with vectors[i*dims .. i*dims+dims) in order. Rejects with RangeError on a reserved id, an id
         * twice in the batch, an id already in the index (checked when the add runs), a length other than
         * ids.length * dims, or a non-finite (or cosine zero) vector, and then leaves the index unchanged; TypeError on
         * wrong array types. Running out of memory part way rejects and keeps the vectors inserted so far.
         */
        add(ids: BigUint64Array | Uint32Array, vectors: Float32Array): Promise<void>;
        /**
         * The k nearest vectors to query, k an integer in [1, 65536]. k above the number of matching vectors returns
         * fewer results; an empty index returns empty arrays. flat f32 is exact; hnsw is approximate unless the exact
         * fallback applies; sq8 and binary are approximate (see VectorQuantization). Rejects with RangeError on a bad k or efSearch, a length other than dims, a non-finite (or
         * cosine zero) query, or a BigUint64Array filter that is not strictly ascending; TypeError on a bad filter or options.
         */
        search(query: Float32Array, k: number, opts?: VectorSearchOptions): Promise<VectorSearchResult>;
        /** dyna-specific. search() for each dims-long row of queries in one call; rejects with the errors search() rejects with. */
        searchBatch(queries: Float32Array, k: number, opts?: VectorSearchOptions): Promise<VectorBatchResult>;
        /** Removes the given ids and returns how many were present (an id listed twice counts once). @throws {TypeError} on wrong array type or while an add or search is unsettled. */
        remove(ids: BigUint64Array | Uint32Array): number;
        /** dyna-specific. Whether id is in the index (removed ids are not); a number or bigint that is not a valid id returns false. @throws {TypeError} when id is not a number or bigint, or while an add or search is unsettled. */
        has(id: number | bigint): boolean;
        /** Writes the index to path; see the class notes. @throws {TypeError} when path is not a dyna:file Path, an option is not a boolean, the index is closed, or an add or search is unsettled. @throws {Error} with .code EPERSIST_* when the write fails. */
        save(path: import("dyna:file").Path, opts?: import("dyna:structures").PersistSaveOptions): import("dyna:structures").PersistSaveResult;
        /** Reads an index written by save; see the class notes. @throws {TypeError} when path is not a dyna:file Path or an option is not a boolean. @throws {Error} with .code EPERSIST_* when the file is missing, damaged, of another type or fails a check. */
        static load(path: import("dyna:file").Path, opts?: import("dyna:structures").PersistLoadOptions): VectorIndex;
    }
}

/** dyna:semver — semver 2.0.0: parse, compare, increment, and ranges with node-semver semantics. */
declare module "dyna:semver" {
    /** A parsed semver 2.0.0 version. */
    interface SemVer {
        major: number;
        minor: number;
        patch: number;
        prerelease: (string | number)[];
        build: string[];
        version: string;
    }

    /** Parses major.minor.patch[-pre][+build]; values above MAX_SAFE throw. */
    function parse(version: string): SemVer;
    /** Whether the string parses at all. */
    function isValid(version: string): boolean;
    /** Trims, strips a leading `=`, returns the normalized version or null. */
    function clean(version: string): string | null;
    /** The first run of digits normalized into X.Y.Z, or null. */
    function coerce(version: string): string | null;
    /** -1 | 0 | 1 by semver precedence (build metadata does not count). */
    function compare(a: string, b: string): -1 | 0 | 1;
    /** Whether the versions are equal. */
    function eq(a: string, b: string): boolean;
    /** Whether the versions differ. */
    function neq(a: string, b: string): boolean;
    /** Whether `a` is greater than `b`. */
    function gt(a: string, b: string): boolean;
    /** Whether `a` is greater than or equal to `b`. */
    function gte(a: string, b: string): boolean;
    /** Whether `a` is less than `b`. */
    function lt(a: string, b: string): boolean;
    /** Whether `a` is less than or equal to `b`. */
    function lte(a: string, b: string): boolean;
    /** Ascending sort by precedence; does not mutate the input. */
    function sort(versions: string[]): string[];
    /** The major field. */
    function major(v: string): number;
    /** The minor field. */
    function minor(v: string): number;
    /** The patch field. */
    function patch(v: string): number;
    /** The prerelease identifiers, or null when there are none. */
    function prerelease(v: string): (string | number)[] | null;
    /** Bumps by major|minor|patch|premajor|preminor|prepatch|prerelease|release; 'release' strips the prerelease and returns null when there is none. */
    function inc(version: string, release: string, identifier?: string): string | null;
    /** One-shot range match; a prerelease matches only ranges naming a prerelease at the same major.minor.patch. */
    function satisfies(version: string, range: string): boolean;
    /** The highest version in the list that satisfies the range, or null. */
    function maxSatisfying(versions: string[], range: string): string | null;
    /** The lowest version in the list that satisfies the range, or null. */
    function minSatisfying(versions: string[], range: string): string | null;
    /** The highest-precedence component that differs ("major" ... "build"), or null when identical. */
    function diff(a: string, b: string): "major" | "minor" | "patch" | "prerelease" | "build" | null;
    /** compare(), with build identifiers breaking a precedence tie (absent build sorts lowest). */
    function compareBuild(a: string, b: string): -1 | 0 | 1;
    /** Whether two ranges (strings or Range objects) intersect, by node-semver's comparator-pairwise rule. */
    function intersects(rangeA: string | Range, rangeB: string | Range): boolean;

    /** A compiled range expression; same prerelease rule as satisfies(); an invalid range is a TypeError. */
    class Range {
        /** A compiled `Range`. */
        constructor(rangeString: string);
        /** Whether the version matches the compiled range. */
        test(version: string): boolean;
        /** The versions that match, preserving input order. */
        filter(versions: string[]): string[];
        /** The highest matching version, or `null`. */
        maxSatisfying(versions: string[]): string | null;
        /** The lowest matching version, or `null`. */
        minSatisfying(versions: string[]): string | null;
        /** Whether this range intersects `other`. */
        intersects(other: string | Range): boolean;
        /** The original range text. */
        readonly source: string;
        /** The number of `||`-separated sets (the observable measure of compilation). */
        readonly setCount: number;
    }
}

/** dyna:serialize — MessagePack, CBOR, BSON, structuredClone; decoders bounded for hostile input. */
declare module "dyna:serialize" {
    /** Options of the MessagePack/CBOR codecs and ValueHash. A non-object value (null and undefined mean none), an unknown key or a non-boolean bigint is a TypeError. */
    interface CodecOptions {
        /** Exact integer mode, false by default (owner decision D8). Decoders: an int beyond +-(2^53-1) becomes a BigInt instead of a Number rounded like JSON.parse. Encoders: an integer Number beyond +-(2^53-1) is written as a float64 instead of an int, so a decoder in bigint mode can tell it from the equal BigInt. */
        bigint?: boolean;
    }
    /** MessagePack encoding; symbols and functions are refused. The decoders cap depth and validate lengths before allocating. By default an integer Number in [-2^63, 2^64) is an int (uint64 from 2^63) and every other Number (fractions, -0, beyond) is a float64; with { bigint: true } only integers within +-(2^53-1) are ints. A BigInt in the int64 or uint64 range is an int in both modes (RangeError beyond). Strings are well-formed UTF-8 in both modes: a lone surrogate encodes as U+FFFD (WTF-8 bytes would not be valid MessagePack str). */
    function MsgPackEncode(value: unknown, opts?: CodecOptions): Uint8Array;
    /** Decodes one MessagePack value; depth-capped, lengths validated before allocation. By default every int is a Number, rounded like JSON.parse beyond +-(2^53-1); { bigint: true } decodes such an int to an exact BigInt. Invalid UTF-8, encoded surrogates included, decodes to U+FFFD per maximal subpart. */
    function MsgPackDecode(bytes: ByteView, opts?: CodecOptions): unknown;
    /** RFC 8949 CBOR with the same walker, bounds and refusals as MessagePack, except that a BigInt beyond the 64-bit int range is written as the shortest tag 2/3 bignum (magnitude up to 131072 bytes) instead of a RangeError. */
    function CBOREncode(value: unknown, opts?: CodecOptions): Uint8Array;
    /** Decodes one CBOR value under the same bounds and the same int and UTF-8 policy; major type 1 reaches -2^64. Tag 2/3 bignums decode like any int: a Number by default (rounded once from the exact value, Infinity past the double range), with { bigint: true } a BigInt beyond +-(2^53-1); the tag must wrap a definite byte string of at most 131072 bytes (SyntaxError / RangeError). */
    function CBORDecode(bytes: ByteView, opts?: CodecOptions): unknown;
    /** CBOR with map keys sorted byte-wise (deterministic form); the same options as CBOREncode. */
    function CBORCanonical(value: unknown, opts?: CodecOptions): Uint8Array;
    /** Canonical CBOR run through XXH64, returned as 16 lowercase hex chars; it inherits the CBOREncode policy and options. A lone surrogate hashes as U+FFFD in both modes. By default an integer Number in [-2^63, 2^64) hashes as an int, so ValueHash(2**60) === ValueHash(2n ** 60n); with { bigint: true } it hashes as a float64 and the two differ. */
    function ValueHash(value: unknown, opts?: CodecOptions): string;
    /** BSON (bsonspec.org 1.1) encoding of one document: a top-level object (not an array, Date, RegExp or bytes) whose own enumerable string keys are written in insertion order.
     *  Numbers: an integer in the int32 range is int32, any other number (and -0) is double; BigInt is int64 (RangeError outside it). Strings are UTF-8 (a lone surrogate becomes U+FFFD).
     *  Date is UTC datetime (RangeError when invalid), RegExp is a regex with its flags mapped to BSON options (i m s u; g y d are dropped), an ArrayBuffer or byte view is binary subtype 0,
     *  ObjectId/Timestamp/Decimal128/Binary/BSONRegExp/MinKey/MaxKey/Code are their BSON types. An undefined property is skipped; undefined or a hole in an array is null.
     *  A key or regex pattern with a NUL character, a function, a symbol and nesting beyond 200 levels throw. */
    function BSONEncode(document: object): Uint8Array;
    /** Decodes exactly one BSON document that spans the whole input. The bytes are untrusted: the document is fully validated first (every length bounded by its container, terminators,
     *  UTF-8 of keys and strings, boolean 0/1, known types, nesting <= 200) and a violation throws TypeError("BSONDecode: <what> at byte <offset>") before any value is built;
     *  decoding runs over a private copy, so nothing reached during decode can change the input under it.
     *  int32 and double are numbers, int64 is a BigInt, binary subtype 0 (and the old subtype 2) is a Uint8Array, other subtypes are Binary, a regex whose options are within i m s u is a RegExp
     *  (otherwise, or when JavaScript rejects the pattern, a BSONRegExp), the deprecated symbol type is a string and undefined is undefined; DBPointer and code-with-scope throw TypeError. */
    function BSONDecode(bytes: ByteView): Record<string, unknown>;
    /** The 12-byte BSON ObjectId: 4-byte big-endian seconds, 5 random bytes fixed per process, 3-byte big-endian counter from a random start. */
    class ObjectId {
        /** No argument generates a fresh id; a 24-character hex string, 12 bytes or another ObjectId copies one (anything else is a TypeError). */
        constructor(id?: string | ByteView | ObjectId);
        /** The 24 lowercase hex characters. */
        toHexString(): string;
        /** The same as toHexString(). */
        toString(): string;
        /** The same as toHexString(), so JSON.stringify writes the hex. */
        toJSON(): string;
        /** True for an ObjectId or a hex string naming the same 12 bytes. */
        equals(other: ObjectId | string): boolean;
        /** The embedded creation time, second precision. */
        getTimestamp(): Date;
        /** A copy of the 12 bytes. */
        readonly id: Uint8Array;
        /** Whether the constructor would accept the value (an absent value is false). */
        static isValid(id: unknown): boolean;
        /** An id whose time is the given seconds and whose other 8 bytes are zero (a range bound for queries by time). */
        static createFromTime(seconds: number): ObjectId;
    }
    /** The BSON timestamp: t seconds and i ordinal, both unsigned 32-bit. */
    class Timestamp {
        /** From { t, i } (integers within 0..2^32-1) or a BigInt whose high word is t and low word is i. */
        constructor(value: { t: number; i: number } | bigint);
        /** The seconds. */
        readonly t: number;
        /** The ordinal within the second. */
        readonly i: number;
        /** "Timestamp(t, i)". */
        toString(): string;
        /** { $timestamp: { t, i } } (MongoDB Extended JSON). */
        toJSON(): { $timestamp: { t: number; i: number } };
    }
    /** IEEE 754-2008 decimal128 (BID encoding), carried exactly; arithmetic is not provided. */
    class Decimal128 {
        /** Parses [-+]digits[.digits][E[-+]digits], Infinity or NaN. More than 34 significant digits that would need rounding, or an exponent outside -6176..6111, is a RangeError; other text is a TypeError. */
        constructor(value: string);
        /** The canonical string of the BSON Decimal128 specification (scientific notation when the exponent is positive or the adjusted exponent is below -6). */
        toString(): string;
        /** { $numberDecimal: string }. */
        toJSON(): { $numberDecimal: string };
        /** The nearest double. */
        toNumber(): number;
    }
    /** BSON binary with a subtype other than 0 (subtype 0 decodes to a plain Uint8Array). */
    class Binary {
        /** Copies the bytes; subType is 0..255 (default 0). */
        constructor(bytes: ByteView, subType?: number);
        /** A copy of the payload. */
        readonly buffer: Uint8Array;
        /** The BSON binary subtype. */
        readonly subType: number;
        /** The payload length in bytes. */
        readonly length: number;
    }
    /** A BSON regex kept as text: options JavaScript has no flag for (l, x), or a pattern JavaScript rejects. */
    class BSONRegExp {
        /** Neither string may contain a NUL character. */
        constructor(pattern: string, options?: string);
        /** The pattern text. */
        readonly pattern: string;
        /** The option letters. */
        readonly options: string;
    }
    /** The BSON MinKey: below every other value in BSON order. */
    class MinKey {
        constructor();
        /** { $minKey: 1 }. */
        toJSON(): { $minKey: 1 };
    }
    /** The BSON MaxKey: above every other value in BSON order. */
    class MaxKey {
        constructor();
        /** { $maxKey: 1 }. */
        toJSON(): { $maxKey: 1 };
    }
    /** BSON JavaScript code (type 0x0D), carried as text and never evaluated. */
    class Code {
        constructor(code: string);
        /** The source text. */
        readonly code: string;
    }
    /** Deep clone (the global structuredClone), HTML StructuredSerializeWithTransfer: preserves cycles and shared refs; accessors are read via [[Get]] into data properties; Error (by name), DOMException, boxed primitives and every view kind (DataView included, subclasses as their intrinsic) clone; functions, symbols, Proxies, detached buffers and other exotic objects throw DataCloneError.
     *  options.transfer: ArrayBuffers are detached after a successful clone, streams move (same thread); a duplicate, a SharedArrayBuffer, a detached buffer or a non-transferable throws DataCloneError, a non-object TypeError. */
    function structuredClone(value: unknown, options?: { transfer?: Iterable<unknown> }): unknown;
}

/** dyna:protobuf — protobuf wire codec over a { fields } schema; decode bounded (nesting <= 64). */
declare module "dyna:protobuf" {
    /** Protobuf wire encoding/decoding driven by a plain-object descriptor. */
    interface ProtoField {
        name: string;
        number: number;
        type: string;
        repeated?: boolean;
        packed?: boolean;
        keyType?: string;
        valueType?: string;
        message?: { fields: ProtoField[] };
    }
    /** Protobuf wire codec driven by a { fields } schema object (no .proto compiler). */
    const Proto: {
        /** Wire encoding, strict about JS types and ranges; `oneof` is an ANNOTATION only (last one wins, as in proto2/3). */
        encode(value: unknown, schema: { fields: ProtoField[] }): Uint8Array;
        /** The untrusted surface: lengths validated, nesting capped at 64. */
        decode(bytes: ByteView, schema: { fields: ProtoField[] }): unknown;
    };
}

/** dyna:asn1 — ASN.1 DER node builders, encode, bounded decode (nesting <= 256). */
declare module "dyna:asn1" {
    /** Canonical DER codec. */
    interface ASN1Node {
        cls: number;
        tag: number;
        constructed: boolean;
        value: unknown;
    }
    /** ASN.1 DER: node builders, encode and decode over one node tree. */
    const ASN1: {
        /** DER-encodes a node tree. */
        encode(node: ASN1Node): Uint8Array;
        /** Untrusted input: nesting <= 256, children per construct <= 65536, total nodes <= 1048576 (RangeError before any proportional allocation). */
        decode(bytes: Uint8Array | ArrayBuffer): ASN1Node;
        /** A SEQUENCE or SET node over the children. */
        seq(children: ASN1Node[]): ASN1Node;
        /** A SEQUENCE or SET node over the children. */
        set(children: ASN1Node[]): ASN1Node;
        /** Any integer Number or BigInt as minimal two's complement (64 content bytes at most); decodes to a Number up to 2^53, a BigInt beyond. */
        int(value: number | bigint): ASN1Node;
        /** A BOOLEAN node. */
        bool(value: boolean): ASN1Node;
        /** A NULL node. */
        null(): ASN1Node;
        /** An OCTET STRING node. */
        octets(bytes: ByteView): ASN1Node;
        /** BIT STRING with `unused` trailing pad bits. */
        bitString(bytes: ByteView, unused: number): ASN1Node;
        /** An OBJECT IDENTIFIER node. */
        oid(str: string): ASN1Node;
        /** A UTF8String, PrintableString, IA5String (tag 22), BMPString (tag 30) or UniversalString (tag 28) node. */
        utf8(str: string): ASN1Node;
        /** IA5String (tag 22): ASCII only; non-ASCII refuses at encode. */
        ia5String(str: string): ASN1Node;
        /** BMPString (tag 30), UCS-2 big-endian; code points above U+FFFF and lone surrogates are refused. */
        bmpString(str: string): ASN1Node;
        /** UniversalString (tag 28), UCS-4 big-endian; lone surrogates are refused. */
        universalString(str: string): ASN1Node;
        /** A UTF8String, PrintableString, IA5String (tag 22), BMPString (tag 30) or UniversalString (tag 28) node. */
        printable(str: string): ASN1Node;
        /** A UTCTime or GeneralizedTime node (the strings are emitted verbatim). */
        utcTime(str: string): ASN1Node;
        /** A UTCTime or GeneralizedTime node (the strings are emitted verbatim). */
        generalizedTime(str: string): ASN1Node;
        /** Context-specific primitive [tag] with raw content. */
        context(tag: number, content: ByteView): ASN1Node;
        /** Context-specific constructed [tag] wrapping child nodes. */
        contextC(tag: number, children: ASN1Node[]): ASN1Node;
    };
}

/** dyna:simd — explicit vector math over typed arrays: reductions, dot products, distances, activations, matrix ops; the best kernel for the CPU is chosen at startup.
 *  Plain names are f32 (F32Like), `f64*` take Float64Array, `i32*` strictly Int32Array; most kernels accept a trailing (offset, length) window.
 *  An Int32Array or Uint32Array given to a plain (f32) kernel is a bit-cast view: its words are read as float32 bit patterns, not as integers (sum([1, 2]) is about 4e-45); integer data goes to the i32* kernels. */
declare module "dyna:simd" {
    // Reductions accumulate in vector lanes: they match a scalar loop to a relative tolerance, not bitwise; NaN propagates (argmax/argmin ignore it).

    /** Arithmetic sum of a float array. */
    function sum(a: F32Like): number;
    /** Largest element (min is the smallest); an array of infinities answers that infinity. */
    function max(a: F32Like): number;
    /** The minimum value. Throws `RangeError` on an empty array. */
    function min(a: F32Like): number;
    /** Index of the extreme; empty throws. Ties resolve to the first occurrence, identically on every backend. */
    function argmax(a: F32Like): number;
    /** The index of the minimum. Throws `RangeError` on an empty array. */
    function argmin(a: F32Like): number;
    /** The 1-norm. */
    function normL1(a: F32Like): number;
    /** The Euclidean norm. */
    function normL2(a: F32Like): number;

    /** Elementwise: out[i] = a[i] op b[i]; all lengths must match. */
    function add(out: F32Like, a: F32Like, b: F32Like): F32Like;
    /** Elementwise out = a - b (mul and div likewise); returns out. */
    function sub(out: F32Like, a: F32Like, b: F32Like): F32Like;
    /** `out[i] = a[i] * b[i]` and returns `out`. All lengths must match. */
    function mul(out: F32Like, a: F32Like, b: F32Like): F32Like;
    /** `out[i] = a[i] / b[i]` and returns `out`. All lengths must match. */
    function div(out: F32Like, a: F32Like, b: F32Like): F32Like;
    /** `out[i] = |a[i]|` and returns `out`. */
    function abs(out: F32Like, a: F32Like): F32Like;
    /** In-place z[i] += a[i] * b[i]. */
    function fma(z: F32Like, a: F32Like, b: F32Like): F32Like;
    /** Inner product. */
    function dot(a: F32Like, b: F32Like): number;

    /** In-place scalar ops. */
    function scale(a: F32Like, s: number): F32Like;
    /** In-place a[i] += s; returns a. */
    function addScalar(a: F32Like, s: number): F32Like;
    /** y[i] += alpha * x[i]. */
    function axpy(y: F32Like, alpha: number, x: F32Like): F32Like;
    /** a[i] = alpha * a[i] + beta. */
    function affine(a: F32Like, alpha: number, beta: number): F32Like;

    /** In-place activations using a fast exponential: SIMD and scalar agree to a relative tolerance of 1e-3 and may differ at saturation. */
    function sigmoid(a: F32Like): F32Like;
    /** In-place `max(x, 0)`; returns `a`. */
    function relu(a: F32Like): F32Like;
    /** In-place `min(max(x, 0), 6)`; returns `a`. */
    function relu6(a: F32Like): F32Like;
    /** In-place `x < 0 ? slope * x : x`; returns `a`. */
    function leakyRelu(a: F32Like, slope: number): F32Like;
    /** In-place `x < 0 ? alpha * (exp(x) - 1) : x`; returns `a`. Uses libm expf per element (exact libm results, no approximation; kept deliberately). */
    function elu(a: F32Like, alpha: number): F32Like;
    /** In-place fast tanh via the sigmoid identity; returns `a`. */
    function tanhFast(a: F32Like): F32Like;
    /** In-place Gaussian error linear unit, tanh form 0.5x(1 + tanh(sqrt(2/pi)(x + 0.044715x^3))) with tanh from libm tanhf (no faster tanh approximation; kept deliberately); returns `a`. */
    function gelu(a: F32Like): F32Like;
    /** In-place `x * sigmoid(x)`, composed from `sigmoid` + `mul` so it is correct on every ISA; returns `a`. */
    function silu(a: F32Like): F32Like;
    /** Stable max-shifted softmax in place; a NaN input answers all-NaN. */
    function softmax(a: F32Like): F32Like;
    /** Log-softmax in place; returns `a`. Throws on an empty array. */
    function logSoftmax(a: F32Like): F32Like;

    /** In-place e^x within 2 ulp on every ISA (range reduction + degree-6 polynomial); below -88 is 0, above 88.7228 is +Infinity, NaN propagates. */
    function vexp(a: F32Like): F32Like;
    /** In-place elementwise ln via libm logf (no approximation; kept deliberately); vsqrt/vrsqrt/vinv are sqrt, 1/sqrt and 1/x, clamped to 0 where undefined (x < 0, x <= 0, x == 0). */
    function vlog(a: F32Like): F32Like;
    /** In-place square root; returns `a`. */
    function vsqrt(a: F32Like): F32Like;
    /** In-place reciprocal square root; returns `a`. */
    function vrsqrt(a: F32Like): F32Like;
    /** In-place reciprocal; returns `a`. */
    function vinv(a: F32Like): F32Like;

    /** Vector-to-scalar distances over equal-length pairs. */
    function distL2(a: F32Like, b: F32Like): number;
    /** Manhattan (L1) distance. */
    function distL1(a: F32Like, b: F32Like): number;
    /** Cosine distance: 1 - cosine similarity (0 for parallel vectors, 1 for orthogonal). */
    function distCos(a: F32Like, b: F32Like): number;
    /** Chebyshev (L-infinity) distance. */
    function distCheb(a: F32Like, b: F32Like): number;

    /** BLAS-2/3 kernels are row-major with explicit dimensions. gemv, 6-argument form: y = beta*y + A*x. */
    function gemv(y: F32Like, a: F32Like, x: F32Like, m: number, n: number, beta: number): F32Like;
    /** 7-argument gemv: y = alpha*A*x + beta*y (beta == 0 skips reading y). */
    function gemv(y: F32Like, a: F32Like, x: F32Like, m: number, n: number, alpha: number, beta: number): F32Like;
    /** Transposed gemv: y = beta*y + A^T*x. */
    function gemvT(y: F32Like, a: F32Like, x: F32Like, m: number, n: number, beta: number): F32Like;
    /** General matrix multiply C = alpha*A*B + beta*C; the output must not share memory with an input. */
    function gemm(c: F32Like, a: F32Like, b: F32Like, m: number, n: number, k: number, alpha: number, beta: number): F32Like;

    // Windows: vector kernels (not gemv/gemvT/gemm/topkIndices) take a trailing (offset, length) or {offset, length|limit}: sum(a, 8, 16). An empty bag {} (or one holding only undefined/null) is the same as no window: the whole array; an offset without length (or limit) is a TypeError "window requires length (or limit)"; length and limit together are a TypeError; any other key is a TypeError naming the valid set.|limit}: sum(a, 8, 16).

    /** Arithmetic mean = sum(a)/n. */
    function mean(a: F32Like, offset?: number, length?: number): number;
    function mean(a: F32Like, opts?: { offset?: number; length?: number; limit?: number }): number;
    /** POPULATION variance (divide by n), two-pass centered; multiply by n/(n-1) for the sample form DataFrame.VARIANCE uses. */
    function variance(a: F32Like): number;
    /** In-place z-score (a[i]-mean)/std; returns a. A constant array has no z-score: the output is all-NaN or rounding noise. */
    function normalize(a: F32Like): F32Like;
    /** Arithmetic mean over Float64Array = f64Sum(a)/n. */
    function f64Mean(a: Float64Array): number;
    /** Two-pass centered population variance over Float64Array. */
    function f64Variance(a: Float64Array): number;
    /** Mean over Int32Array with exact int64 accumulation and one final rounding. */
    function i32Mean(a: Int32Array): number;

    /** In-place min(max(x, lo), hi). */
    function clamp(a: F32Like, lo: number, hi: number): F32Like;
    /** In-place binarise: x > t ? 1.0 : 0.0. */
    function threshold(a: F32Like, t: number): F32Like;
    /** Indices of the k largest values, best first (a fresh array): descending value, NaN last, equal values by lower index. */
    function topkIndices(vals: F32Like, k: number): Uint32Array;

    /** Zero-copy Float64Array kernels. */
    function f64Sum(a: Float64Array): number;
    /** The inner product. Reorders additions and rounds slightly differently from a sequential sum. */
    function f64Dot(a: Float64Array, b: Float64Array): number;
    /** The maximum; throws on an empty array. Bit-exact. */
    function f64Max(a: Float64Array): number;
    /** The minimum; throws on an empty array. Bit-exact. */
    function f64Min(a: Float64Array): number;
    /** In-place `a[i] *= s`; returns `a`. Bit-exact. */
    function f64Scale(a: Float64Array, s: number): Float64Array;
    /** In-place y += alpha*x; returns y. */
    function f64Axpy(y: Float64Array, alpha: number, x: Float64Array): Float64Array;

    /** Zero-copy Int32Array kernels only. */
    function i32Sum(a: Int32Array): number;
    /** Exact minimum; throws on an empty array. */
    function i32Min(a: Int32Array): number;
    /** Exact maximum; throws on an empty array. */
    function i32Max(a: Int32Array): number;
    /** Sum of double products; matches a sequential dot to a relative tolerance. */
    function i32Dot(a: Int32Array, b: Int32Array): number;
    /** Writes into a separate `out`, wrapping per element (mod 2³², like `(a+b)|0`); returns `out`. */
    function i32Add(out: Int32Array, a: Int32Array, b: Int32Array): Int32Array;
    /** Writes into a separate `out`, keeping the low 32 bits per element (like `Math.imul`); returns `out`. */
    function i32Mul(out: Int32Array, a: Int32Array, b: Int32Array): Int32Array;
    /** In-place low-32-bit multiply; returns `a`. */
    function i32Scale(a: Int32Array, s: number): Int32Array;

    /** Inclusive prefix scan, in place (the output type follows the input). */
    function cumsum(a: Int32Array): Int32Array;
    /** out[i] = src[idx[i]] (raw 32-bit copy; f64Gather for Float64Array); every index is checked before the first write, out may not overlap src or idx. */
    function gather(out: F32Like, src: F32Like, idx: Int32Array): F32Like;
    function f64Gather(out: Float64Array, src: Float64Array, idx: Int32Array): Float64Array;
    /** dst[idx[i]] += vals[i] in index order (Float32Array only; f64ScatterAdd for Float64Array); indices checked first, dst may not overlap idx or vals. */
    function scatterAdd(dst: Float32Array, idx: Int32Array, vals: Float32Array): Float32Array;
    function f64ScatterAdd(dst: Float64Array, idx: Int32Array, vals: Float64Array): Float64Array;
    /** dst[i] += src[idx[i]] for i in index order (Float32Array only, one float32 rounding per add; f64GatherAdd for Float64Array); dst.length === idx.length, every index is checked against src before the first write, dst may not overlap src or idx. The gather half of a sparse update (embedding-bag sums, CSR row pulls). */
    function gatherAdd(dst: Float32Array, src: Float32Array, idx: Int32Array): Float32Array;
    function f64GatherAdd(dst: Float64Array, src: Float64Array, idx: Int32Array): Float64Array;
    /** Sparse matrix-vector product y = A*x, A in dyna:ml's CSR (scipy) layout: row r holds values[k] at columns[k] for k in [rowPointers[r], rowPointers[r+1]); duplicate columns in a row sum. y.length is the row count, so rowPointers.length === y.length + 1, rowPointers[0] === 0, non-decreasing, ending at values.length === columns.length; every column is checked against x.length before y is written (RangeError otherwise). y may not overlap any input. Each row sums its products left to right in float64 and rounds once to float32 (f64Spmv: the plain float64 left-to-right sum, bit-identical to a JS loop). Overwrites y; returns y. */
    function spmv(y: Float32Array, values: Float32Array, columns: Int32Array, rowPointers: Int32Array, x: Float32Array): Float32Array;
    function f64Spmv(y: Float64Array, values: Float64Array, columns: Int32Array, rowPointers: Int32Array, x: Float64Array): Float64Array;
    /** Float32 accumulates in float32: over long inputs the running sum drifts (about 1.5% across a window 360 000 samples in), so differences of prefix sums need the Float64Array form. */
    function cumsum(a: Float32Array): Float32Array;
    /** Left-to-right float64 accumulation, in place; every element is the exactly-ordered running sum. */
    function cumsum(a: Float64Array): Float64Array;
    /** Inclusive prefix maximum, in place; returns `a`. Exact for both (max rounds nothing). */
    function cummax(a: Int32Array): Int32Array;
    function cummax(a: Float32Array): Float32Array;
}

/** dyna:ssh — an SSH-2 client and server (RFC 4251-4254) in native C on the shared reactor, with OpenSSL for the primitives. The transport takes
 *  only the best algorithms and nothing else is negotiable: key exchange mlkem768x25519-sha256 (ML-KEM-768 + X25519, the shared secret
 *  SHA-256(K_pq || K_x25519) entering the exchange hash as a string), then sntrup761x25519-sha512 (also offered as
 *  sntrup761x25519-sha512@openssh.com: Streamlined NTRU Prime 761 + X25519, SHA-512(K_pq || K_x25519) as a string, SHA-512 exchange hash and
 *  key derivation; the sntrup761 code is this module's own constant-time implementation of the NTRU Prime round-3 KEM), then
 *  curve25519-sha256 (also offered as curve25519-sha256@libssh.org); host keys ssh-ed25519-cert-v01@openssh.com, ssh-ed25519 and
 *  sk-ssh-ed25519@openssh.com (verify only); ciphers chacha20-poly1305@openssh.com then aes256-gcm@openssh.com (AEAD: the MAC name-list is
 *  sent empty); compression "none". CBC/CTR, SHA-1 key exchange, DSA/ECDSA, non-AEAD MACs, compression, SSH-1 and the none cipher do not
 *  exist in the code. The client picks its first algorithm the server offers; the server picks the client's first algorithm it supports.
 *  Strict key exchange (kex-strict-c/s-v00@openssh.com, the Terrapin countermeasure) is required in both roles: a peer whose first KEXINIT
 *  lacks the other side's marker is refused, the first packet must be KEXINIT, any other message during the initial exchange is fatal, and
 *  sequence numbers restart at zero after every NEWKEYS. The client offers ext-info-c (RFC 8308) and records the server's server-sig-algs;
 *  the server sends EXT_INFO (server-sig-algs "ssh-ed25519,sk-ssh-ed25519@openssh.com") right after its first NEWKEYS when the client
 *  offered ext-info-c. Either side may re-key at any time; each side starts one after dyna.rekeyBytes bytes (both directions, default 1 GiB;
 *  checked on every sent and received packet, so a side overshoots the limit by at most one packet) or dyna.rekeySeconds (default 3600);
 *  channel data waits in its channel while a re-key runs and is sent after NEWKEYS, and the host key must stay
 *  the same as the first exchange's. Every fatal error sends SSH_MSG_DISCONNECT with the RFC 4253 reason code (2 protocol error, 3 key
 *  exchange failed, 5 MAC error, 7 service not available, 9 host key not verifiable, 11 by application, 14 no more auth methods, 15 illegal
 *  user name) and the message the rejection carries.
 *  Authentication is publickey only, in both roles: ssh-ed25519 keys in the OpenSSH format (openssh-key-v1, unencrypted, or encrypted by
 *  ssh-keygen with bcrypt-pbkdf and aes256-ctr or aes256-gcm@openssh.com: those ciphers decrypt the key file only and are never offered on
 *  the wire), sk-ssh-ed25519@openssh.com signatures (server side, user-presence flag required) and OpenSSH ed25519 certificates
 *  (ssh-ed25519-cert-v01@openssh.com, PROTOCOL.certkeys) signed by an ssh-ed25519 CA. There is no password, keyboard-interactive, hostbased
 *  or GSSAPI code. A certificate is accepted only when its CA signature verifies, its type is right (user for SSHServer logins, host for a
 *  server's host key), now is inside [valid_after, valid_before), its principals list names the user (or the host name the client dialled),
 *  and every critical option is understood: force-command (user certificates; the session's `command` becomes it) and source-address
 *  (user certificates; comma-separated CIDRs the client address must match). Any other critical option (verify-required included), and any
 *  critical option on a host certificate, refuses the certificate. The permit-pty extension is enforced (no pty-req without it); other
 *  extensions are ignored.
 *  The client checks the server's host key against an OpenSSH known_hosts file: plain and hashed (|1|salt|hash, HMAC-SHA1 by that format;
 *  used only to match a host name at rest) entries, comma-separated patterns with * ? and ! negation, [host]:port for a port other than 22,
 *  @revoked lines (a match refuses, for a host key or a certificate's key or CA) and @cert-authority lines (an ssh-ed25519 CA trusted for
 *  the matching hosts; the client offers the host-certificate algorithm only when such a line matches the host). An unknown host is
 *  refused by default. */
declare module "dyna:ssh" {
    /** dyna extension beyond the SSH RFCs: meta configuration in SSHConnectOptions.dyna. A strict bag (an unknown key is a TypeError naming
     *  the valid ones). It only disables costly behaviour or moves safety caps; the protocol defaults above never change with it.
     *  A non-number cap is a TypeError, a fraction or an out-of-range value a RangeError. */
    interface SSHMetaOptions {
        /** "strict" (default): a host absent from known_hosts is refused. "accept-new": an unknown host's key is appended to the
         *  knownHosts file as a HASHED entry ("|1|base64(salt)|base64(HMAC-SHA1(salt, host)) type base64", 20 random salt bytes, the
         *  host written "[host]:port" for a port other than 22; mode 0600 on creation) and trusted; a CHANGED or @revoked key is still
         *  refused. A host certificate is never appended: it needs a matching @cert-authority line. */
        hostKeyPolicy?: "strict" | "accept-new";
        /** Default false. true adds rsa-sha2-512 host keys (RSA PKCS#1 v1.5 over SHA-512, 2048..16384-bit moduli) after the ed25519 ones for
         *  a legacy server; ssh-rsa (SHA-1) is never offered. */
        allowRsaSha2_512?: boolean;
        /** Largest incoming packet_length accepted, 35000..16777216; default 262144. A bigger packet ends the connection (protocol error). */
        maxPacketBytes?: number;
        /** Per-channel receive window, 65536..1073741824; default 2097152. It bounds the bytes buffered per exec stream: the window is granted
         *  back only as the stdout/stderr readers drain their queues. */
        windowBytes?: number;
        /** Channels open at once on one connection, 1..4096; default 64. exec() past it rejects; a forwarded connection past it is closed. */
        maxChannels?: number;
        /** Connect + key exchange + authentication deadline in ms, 1..600000; default 30000. */
        handshakeTimeoutMs?: number;
        /** Bytes (in plus out) after which the client re-keys, 65536..2^32; default 2^30. */
        rekeyBytes?: number;
        /** Seconds after which the client re-keys, 1..86400; default 3600. */
        rekeySeconds?: number;
        /** Default false: a connection keeps the capacity its buffers grew to (socket input, packet output, each channel's outgoing
         *  queue) and reuses it, the fastest choice for steady transfers. true frees a buffer above 256 KiB as soon as it is empty, so an
         *  idle connection after a large burst gives that capacity back (measured: 4 MiB exec bursts left about 6.4 MiB per connection pair
         *  held, against about 0.3 MiB with it; idle forwarded TCP connections hold small buffers either way); it costs about 6..9% CPU on a sustained transfer between
         *  two dyna peers (buffers are re-grown after every drain). Visible in stats.bufferBytes. */
        shrinkBuffers?: boolean;
    }

    /** Options for SSHClient.connect; a strict bag (an unknown key is a TypeError naming the valid ones). */
    interface SSHConnectOptions {
        /** Host name or address (resolved off the event loop). Also the name looked up in known_hosts and the principal a host
         *  certificate must list. */
        host: string;
        /** Default 22. */
        port?: number;
        username: string;
        /** OpenSSH ssh-ed25519 private key text (string or bytes, <= 64 KiB); bad key/type/passphrase: sync TypeError.
         *  Required unless `agent`; with both, tried first and the agent only after the server refused it. */
        privateKey?: string | ArrayBuffer | ArrayBufferView;
        /** Passphrase of an encrypted key; bcrypt-pbkdf rounds > 4096 refused; without privateKey a TypeError. */
        passphrase?: string;
        /** ssh-agent socket path, or true for $SSH_AUTH_SOCK (never implicit; unset, too long or other type: sync TypeError). Keys queried
         *  before signing; all refused: reject "refused for every key tried"; bad agent reply: reject naming the path; hung: handshakeTimeoutMs. */
        agent?: string | true;
        /** An OpenSSH user certificate for privateKey (the "ssh-ed25519-cert-v01@openssh.com AAAA... [comment]" line ssh-keygen -s writes
         *  to key-cert.pub). When set, the client authenticates with the certificate instead of the bare key. A certificate for another
         *  key, a host certificate or a malformed line is a synchronous TypeError. */
        certificate?: string;
        /** Path of the known_hosts file; default $HOME/.ssh/known_hosts. A missing file means every host is unknown. */
        knownHosts?: string;
        /** ProxyJump (OpenSSH -J): a connected SSHClient to tunnel through. The new connection's transport is a direct-tcpip channel
         *  on `via` to host:port (as the jump host resolves it; no local DNS), so the jump server's forward policy applies. Everything
         *  else is end to end: the key exchange, the host key check (against THIS knownHosts entry for host:port, never the jump's),
         *  and authentication. Chains nest (via a client that itself has a via). Closing the inner client leaves the jump usable;
         *  the jump ending ends the inner client ("the jump connection closed"). Not an SSHClient, or not connected: a TypeError. */
        via?: SSHClient;
        /** dyna extension (see SSHMetaOptions). */
        dyna?: SSHMetaOptions;
    }

    /** Options for SSHClient.forwardLocal / forwardRemote; a strict bag. host/port name the TARGET (1..255 bytes; port 1..65535), the
     *  listening end is localHost/localPort (forwardLocal) or remoteHost/remotePort (forwardRemote), default "127.0.0.1" and 0 (an
     *  ephemeral port, read back from listenPort). The other side's pair (remoteHost on forwardLocal, ...) is a TypeError; a bad
     *  port is a RangeError. */
    interface SSHForwardOptions {
        host: string;
        port: number;
        localHost?: string;
        localPort?: number;
        remoteHost?: string;
        remotePort?: number;
    }

    /** One port forward on an SSHClient. Bytes are relayed in C between each TCP connection and its SSH channel, with backpressure both
     *  ways: the socket is read only while the channel's outgoing queue is below 256 KiB (read again at 64 KiB), and the channel window
     *  is granted back only as bytes reach the socket. Half-close passes through (a socket EOF sends CHANNEL_EOF; a CHANNEL_EOF shuts
     *  the socket's write side once the queued bytes are written), and a CHANNEL_CLOSE that arrives while bytes are still queued to the
     *  socket closes it only after they are written (at most dyna.maxChannels such draining sockets per connection, past that one is
     *  closed at once; a socket that never reads keeps its bytes until the SSH connection ends, which closes it). A forward stays alive while the connection does, even when the object is dropped;
     *  the connection ending closes it (and every relayed socket). */
    interface SSHForward {
        /** "local": this process listens and opens a direct-tcpip channel per accepted connection (OpenSSH -L). "remote": the server
         *  listens (tcpip-forward) and opens a forwarded-tcpip channel per connection; this side connects it to the target (-R). */
        readonly type: "local" | "remote";
        /** Where the listener is: the local bind address, or for "remote" the address string sent to the server. */
        readonly listenHost: string;
        /** The bound port (for "remote" with remotePort 0, the port the server allocated and returned in its reply). */
        readonly listenPort: number;
        readonly targetHost: string;
        readonly targetPort: number;
        /** Connections relayed so far. */
        readonly connections: number;
        /** Stops listening (a remote forward sends cancel-tcpip-forward); connections already relayed keep running. Idempotent. */
        close(): void;
        readonly closed: boolean;
    }

    /** What SSHExec.exit and SSHSession.done resolve with. It never rejects: a connection lost first resolves with exitCode null and
     *  `error` set. */
    interface SSHExitStatus {
        /** The exit-status sent, or null (a signal, or none sent). */
        exitCode: number | null;
        /** The exit-signal name without "SIG" ("TERM", "KILL", ...), or null. */
        signal: string | null;
        coreDumped: boolean;
        /** The exit-signal's error message, or "". */
        message: string;
        /** Why the channel ended abnormally (connection lost, client or server closed), or null for an orderly close. */
        error: string | null;
    }

    /** One "session" channel running one command (RFC 4254 exec). The streams are WHATWG ReadableStreams of Uint8Array chunks (one per
     *  SSH data packet) with a byte-length queuing strategy of dyna.windowBytes: the receive window is re-granted to the server (one
     *  WINDOW_ADJUST once half the window is free) only as the queues drain, so an unread stream stops the remote command instead of
     *  growing memory; stdout and stderr share the channel window, so read both (or cancel the one not wanted). The server's EOF closes
     *  both streams; a lost connection errors them. A cancelled stream drops further data for it and keeps granting its share of the
     *  window. The channel stays referenced by the connection until it closes, so `exit` settles even when the object is dropped. */
    interface SSHExec {
        readonly stdout: ReadableStream<Uint8Array>;
        /** Extended data type 1 (stderr); other extended data types are discarded. */
        readonly stderr: ReadableStream<Uint8Array>;
        /** Settles once the channel is closed by both sides; exit-status / exit-signal arrive before that. */
        readonly exit: Promise<SSHExitStatus>;
        /** Queues bytes (a string is UTF-8) for the command's stdin; resolves once they are handed to the transport within the server's window
         *  and max packet size, rejects once the channel closed first. More than 256 MiB queued rejects. After end() it rejects. */
        write(data: string | ArrayBuffer | ArrayBufferView): Promise<void>;
        /** Sends EOF on stdin once the queued bytes are out. */
        end(): void;
        /** Sends CHANNEL_CLOSE (after the open completes when still opening); `exit` then settles. */
        close(): void;
        /** Sends a "signal" channel request (RFC 4254 6.9, no reply) with the name without "SIG" ("TERM", "INT", "HUP", ...; "SIGTERM" is
         *  accepted and sent as "TERM"). A name that is not 1..20 of [A-Z0-9] is a TypeError. The server may ignore it (OpenSSH sshd does
         *  for exec channels without a pty). */
        signal(name: string): void;
    }

    /** An authenticated SSH connection. A connected client holds the event loop (and itself) alive until close() or until the connection
     *  ends; close() sends DISCONNECT (by application) and settles every open channel with error "the client was closed". When the
     *  process exits with a connection still live (std.exit, an uncaught error), the runtime releases it and its SSHExec objects without
     *  calling back into JS; the exit code is the script's. */
    class SSHClient implements DynResource {
        /** new SSHClient() is a TypeError: use connect. */
        constructor();
        /** Connects, exchanges keys, verifies the host key, authenticates with the ed25519 key, and resolves with the client. Option errors
         *  throw synchronously; network, protocol, host-key and authentication failures reject with an Error whose message starts "SSH: ". */
        static connect(options: SSHConnectOptions): Promise<SSHClient>;
        /** Opens a session channel and runs `command`; resolves once the server accepted the exec request, rejects when the channel or
         *  the request is refused, the connection is not ready, or dyna.maxChannels are open. */
        exec(command: string): Promise<SSHExec>;
        /** Starts a client-initiated re-key now (no-op while one runs); TypeError before authentication. */
        rekey(): void;
        /** OpenSSH -L: listens on localHost:localPort now (synchronous; a bind error throws an Error starting "SSH: ") and, per accepted
         *  connection, opens a direct-tcpip channel to host:port carrying the peer's address as originator. A refused channel (the
         *  server's policy, or the target unreachable from the server) closes that connection with no bytes. Throws when the connection
         *  is not ready. */
        forwardLocal(options: SSHForwardOptions): SSHForward;
        /** OpenSSH -R: sends tcpip-forward for remoteHost:remotePort and resolves once the server accepts (remotePort 0: listenPort is
         *  the port the server chose); rejects with "SSH: " when the server refuses, or the connection ends first. Each connection the
         *  server reports (forwarded-tcpip, matched by the bound port) is connected from here to host:port; a failed connect refuses
         *  the channel. Where the server binds is its decision (OpenSSH: GatewayPorts; this module's server: publicForwardBinds). */
        forwardRemote(options: SSHForwardOptions): Promise<SSHForward>;
        /** The verified host key: {type, fingerprint "SHA256:<base64>"} (ssh-keygen -l format; for a host certificate, the type is
         *  "ssh-ed25519-cert-v01@openssh.com" and the fingerprint is the certified key's), or null before the key exchange. */
        readonly hostKey: { type: string; fingerprint: string } | null;
        /** The negotiated algorithms, or null before the first key exchange. kex is the canonical name ("sntrup761x25519-sha512" also
         *  when the @openssh.com alias was negotiated). */
        readonly algorithms: { kex: string; hostKey: string; cipherClientToServer: string; cipherServerToClient: string; strictKex: boolean } | null;
        /** The server's EXT_INFO server-sig-algs list, or null when it sent none. */
        readonly serverSignatureAlgorithms: string[] | null;
        /** Wire counters (bytes include packet framing and tags), the number of completed re-keys, and bufferBytes: the capacity of the
         *  buffers this connection holds now (socket input, packet output, pending packets, each open channel's outgoing queues). */
        readonly stats: { bytesIn: number; bytesOut: number; packetsIn: number; packetsOut: number; rekeys: number; openChannels: number; bufferBytes: number };
        /** The server's identification line (e.g. "SSH-2.0-OpenSSH_10.5"), or null. */
        readonly serverVersion: string | null;
        /** The last USERAUTH_BANNER text (at most 64 KiB), or null. */
        readonly banner: string | null;
        /** True while authenticated and not ended. */
        readonly connected: boolean;
        close(): void;
        dispose(): void;
        readonly closed: boolean;
        readonly [Symbol.dispose]: () => void;
    }

    /** dyna extension beyond the SSH RFCs: SSHServerOptions.dyna, a strict bag of safety caps. Each default is a safe limit for an
     *  internet-facing server; raise one only on purpose. A non-number is a TypeError, a fraction or an out-of-range value a RangeError. */
    interface SSHServerMetaOptions {
        /** Connections open at once (any state), 1..65536; default 64. A connection accepted past it is closed at once and counted in
         *  stats.refused. */
        maxConnections?: number;
        /** Failed publickey attempts (a refused query, a bad signature, a refused key, a non-publickey method; "none" does not count)
         *  before DISCONNECT 14, 1..100; default 6. */
        maxAuthTries?: number;
        /** Channels open at once per connection (session and direct-tcpip alike), 1..4096; default 10. CHANNEL_OPEN past it gets OPEN_FAILURE 4
         *  (resource shortage); for a local forward, that connection is closed with no bytes. */
        maxChannels?: number;
        /** Per-channel receive window (the client's stdin buffered for the session.stdin reader), 65536..1073741824; default 2097152. */
        windowBytes?: number;
        /** Largest incoming packet_length accepted, 35000..16777216; default 262144. */
        maxPacketBytes?: number;
        /** Identification + key exchange + authentication deadline per connection in ms, 1..600000; default 30000. */
        handshakeTimeoutMs?: number;
        /** Disconnect an authenticated connection that received nothing for this many ms, 0..604800000; default 0 (off). */
        idleTimeoutMs?: number;
        /** Bytes (in plus out) after which the server re-keys a connection, 65536..2^32; default 2^30. */
        rekeyBytes?: number;
        /** Seconds after which the server re-keys a connection, 1..86400; default 3600. */
        rekeySeconds?: number;
        /** Default false. true frees each connection's buffers above 256 KiB as soon as they are empty (see SSHMetaOptions.shrinkBuffers):
         *  less memory per idle connection after a burst, some CPU per drain on sustained transfers. */
        shrinkBuffers?: boolean;
        /** Default false: every remote forward (tcpip-forward) binds 127.0.0.1 whatever address the client asked for (OpenSSH's
         *  GatewayPorts no), and the reply still echoes the client's address string. true binds the requested address: "", "*",
         *  "0.0.0.0" and "::" mean every interface, "localhost" stays 127.0.0.1 (GatewayPorts clientspecified). */
        publicForwardBinds?: boolean;
    }

    /** What SSHServerOptions.forward receives, once per direct-tcpip channel (type "local") and per tcpip-forward request ("remote"). */
    interface SSHForwardRequest {
        type: "local" | "remote";
        username: string;
        /** "local": the target the client wants this server to connect to. "remote": the address string and port the client asked this
         *  server to listen on (port 0: any). */
        host: string;
        port: number;
        /** "local": the originator address and port the client reported (informational, unverified); "remote": the client's address and 0. */
        originatorHost: string;
        originatorPort: number;
        remoteAddress: string;
        keyFingerprint: string;
    }

    /** What SSHServerOptions.authorize receives, after the client proved possession of the key (the signature verified). */
    interface SSHAuthRequest {
        username: string;
        /** The publickey algorithm: "ssh-ed25519", "sk-ssh-ed25519@openssh.com" or "ssh-ed25519-cert-v01@openssh.com". */
        keyType: string;
        /** SHA256 fingerprint of the key (for a certificate, of the certified key). */
        fingerprint: string;
        /** The key as an authorized_keys line without comment ("ssh-ed25519 AAAA..."); for a certificate, the certified key. */
        publicKey: string;
        /** True when the key is in authorizedKeys; always true for a certificate (one that fails any check against trustedUserCAKeys is
         *  refused before the hook is called, so the hook cannot override a certificate's CA, validity, principals or critical options). */
        listed: boolean;
        /** The parsed certificate (it passed every check), or null for a plain key. */
        certificate: SSHCertificateInfo | null;
        remoteAddress: string;
    }

    interface SSHCertificateInfo {
        type: "user" | "host";
        keyId: string;
        /** Number (exact below 2^53; Infinity at or above). */
        serial: number;
        principals: string[];
        /** Seconds since the epoch; validBefore is Infinity for "forever". */
        validAfter: number;
        validBefore: number;
        caFingerprint: string;
    }

    /** Options for SSHServer.listen; a strict bag (an unknown key is a TypeError naming the valid ones). Every error is a synchronous
     *  TypeError or RangeError naming the option. */
    interface SSHServerOptions {
        /** Address to bind; default "127.0.0.1" (pass "0.0.0.0" or "::" to listen on every interface). */
        host?: string;
        /** Default 0: an ephemeral port, read back from server.port. */
        port?: number;
        /** The host's OpenSSH ssh-ed25519 private key text, as a string or bytes (same rules as SSHConnectOptions.privateKey). */
        hostKey: string | ArrayBuffer | ArrayBufferView;
        hostKeyPassphrase?: string;
        /** An OpenSSH host certificate for hostKey ("ssh-ed25519-cert-v01@openssh.com AAAA..."); when set, the server also offers the
         *  ssh-ed25519-cert-v01@openssh.com host key algorithm (first) and sends the certificate to clients that pick it. */
        hostCertificate?: string;
        /** authorized_keys text: one "ssh-ed25519|sk-ssh-ed25519@openssh.com <base64> [comment]" per line; blank and # lines are skipped.
         *  A line with key options (from=, command=, ...) is a TypeError: express such policy in `authorize`. */
        authorizedKeys?: string;
        /** CA public keys in the same format; a user certificate signed by one of them is accepted under the rules in the module doc. */
        trustedUserCAKeys?: string;
        /** Optional decision hook. Without it, a key is accepted when `listed`. With it, every plain key whose signature verified, and
         *  every certificate that passed all certificate checks, is passed in and the hook decides: return true / false, or a Promise of one (the connection waits; a rejection or a non-boolean means false).
         *  A thrown error means false. Signature-less queries get PK_OK when the key is listed or (for a plain key) a hook is set. At least one of
         *  authorizedKeys, trustedUserCAKeys and authorize is required. */
        authorize?: (req: SSHAuthRequest) => boolean | Promise<boolean>;
        /** Called once per session channel, when the client sends "exec" or "shell" (the CHANNEL_SUCCESS reply is sent first). If it
         *  returns a Promise, the session exits with status 0 when it fulfils, or writes the reason to stderr and exits 1 when it rejects,
         *  unless exit()/kill()/close() came first. A synchronous throw does the same as a rejection. A sync return leaves the session open
         *  until exit(), kill() or close(). */
        onSession: (session: SSHSession) => void | Promise<void>;
        /** USERAUTH_BANNER text sent once per connection before the first authentication reply; at most 64 KiB. */
        banner?: string;
        /** Port forwarding policy. Without it every direct-tcpip channel gets OPEN_FAILURE 1 (administratively prohibited) and every
         *  tcpip-forward REQUEST_FAILURE. With it, each request is passed in and the hook decides: true / false, or a Promise of one
         *  (the channel stays pending, the global request replies stay in order); a throw, a rejection or a non-boolean means false. A
         *  user certificate without permit-port-forwarding is refused before the hook. Allowed "local" channels connect from here to
         *  host:port (a failed connect: OPEN_FAILURE 2 connect failed). Allowed "remote" requests listen (see
         *  SSHServerMetaOptions.publicForwardBinds), at most 16 per connection, and the reply carries the bound port when port 0 was
         *  asked; cancel-tcpip-forward stops one. Listeners and relayed sockets close when their connection ends. ProxyJump (OpenSSH -J)
         *  through this server is a "local" request. */
        forward?: (req: SSHForwardRequest) => boolean | Promise<boolean>;
        /** dyna extension (see SSHServerMetaOptions). */
        dyna?: SSHServerMetaOptions;
    }

    /** One session channel on an SSHServer. Requests the server honours before exec/shell: pty-req (refused for a certificate without
     *  permit-pty; at most one), env (only LANG and LC_* names of [A-Z0-9_], values up to 1024 bytes, at most 64), then exactly one exec or
     *  shell or subsystem (a name of 1..64 bytes; dyna:sftp serves "sftp"). After it: signal (calls onsignal), window-change (updates pty,
     *  calls onresize). x11-req, auth-agent-req and every other request get CHANNEL_FAILURE; port forwarding is decided by SSHServerOptions.forward. No process is spawned: the session is
     *  what onSession does with it. */
    interface SSHSession {
        readonly user: string;
        readonly type: "exec" | "shell" | "subsystem";
        /** The command to run: a certificate's force-command when present, else the exec command or the subsystem name; null for a plain shell. */
        readonly command: string | null;
        /** The command (exec) or subsystem name the client asked for, or null (shell). */
        readonly requestedCommand: string | null;
        /** The accepted env requests. */
        readonly env: Record<string, string>;
        /** The pty-req values (updated sizes arrive through onresize), or null. */
        readonly pty: { term: string; cols: number; rows: number; width: number; height: number } | null;
        readonly remoteAddress: string;
        readonly keyType: string;
        readonly keyFingerprint: string;
        readonly certificate: SSHCertificateInfo | null;
        /** The client's stdin (CHANNEL_DATA), with the same window-driven backpressure as SSHExec.stdout; closes on the client's EOF. */
        readonly stdin: ReadableStream<Uint8Array>;
        /** Settles once the channel is closed by both sides, with what this side sent (exitCode / signal), or error when the connection died. */
        readonly done: Promise<SSHExitStatus>;
        /** Queue stdout bytes (CHANNEL_DATA) / stderr bytes (EXTENDED_DATA 1); resolve once handed to the transport within the client's
         *  window, reject when the session's output is closed. More than 256 MiB queued per stream rejects. */
        write(data: string | ArrayBuffer | ArrayBufferView): Promise<void>;
        writeStderr(data: string | ArrayBuffer | ArrayBufferView): Promise<void>;
        /** Sends EOF once the queued output is out. */
        end(): void;
        /** Once the queued output is out: EOF, exit-status `code` (default 0, 0..2^32-1), then CHANNEL_CLOSE. Later calls are no-ops. */
        exit(code?: number): void;
        /** Like exit, with exit-signal `signal` (default "TERM"; "SIG" prefix stripped; 1..20 of [A-Z0-9]) and an optional message. */
        kill(signal?: string, message?: string): void;
        /** CHANNEL_CLOSE now, without exit status; queued output is dropped. */
        close(): void;
        /** Set to receive the client's "signal" requests (name without "SIG"). */
        onsignal: ((name: string) => void) | null;
        /** Set to receive "window-change" requests. */
        onresize: ((size: { cols: number; rows: number; width: number; height: number }) => void) | null;
    }

    /** A listening SSH server (publickey and certificate logins only). It holds the event loop alive until close(); close() stops
     *  accepting and ends every connection with DISCONNECT 11 "the server was closed" (open sessions' done settle with that error). */
    class SSHServer implements DynResource {
        /** new SSHServer() is a TypeError: use listen. */
        constructor();
        /** Binds, starts accepting, and returns the server. */
        static listen(options: SSHServerOptions): SSHServer;
        readonly port: number;
        readonly host: string;
        /** accepted / refused (past maxConnections or after close) connections so far; connections, authenticated and channels open now. */
        readonly stats: { accepted: number; refused: number; connections: number; authenticated: number; channels: number };
        close(): void;
        dispose(): void;
        readonly closed: boolean;
        readonly [Symbol.dispose]: () => void;
    }

    /** A key for SSHAgent.listen keys / add; strict bag. */
    interface SSHAgentKey {
        /** OpenSSH ssh-ed25519 private key text, as SSHConnectOptions.privateKey. */
        privateKey: string | ArrayBuffer | ArrayBufferView;
        /** Decrypts privateKey once; the agent keeps the decrypted key in memory (wiped on remove, expiry, close). */
        passphrase?: string;
        /** ssh-ed25519-cert-v01 USER certificate line for privateKey; held instead of the bare key. Host cert or other key: TypeError. */
        certificate?: string;
        /** Default "". */
        comment?: string;
        /** 1..31536000 s (ssh-add -t); default forever; checked on every request and list(). */
        lifetimeSeconds?: number;
    }

    /** One identity held by an SSHAgent. */
    interface SSHAgentIdentity {
        type: "ssh-ed25519" | "ssh-ed25519-cert-v01@openssh.com" | "sk-ssh-ed25519@openssh.com";
        /** "type AAAA..." line without comment. */
        publicKey: string;
        /** "SHA256:..." as ssh-keygen -l. */
        fingerprint: string;
        comment: string;
        /** Epoch ms, or null. */
        expiresAt: number | null;
        /** false for sk-ssh-ed25519 (listed; SIGN_REQUEST fails: no FIDO access, owner decision). */
        canSign: boolean;
    }

    /** SSHAgent.listen options; strict bag. */
    interface SSHAgentOptions {
        /** Unix socket to create: an existing path or one over 103/107 bytes (macOS/Linux) is a TypeError. Mode 0600 (umask 0177);
         *  unlinked by close(), runtime shutdown and process exit (not SIGKILL). */
        path: string;
        /** Keys held from the start (see add). */
        keys?: SSHAgentKey[];
        dyna?: {
            /** 1..65536, default 256; past it ADD fails (wire FAILURE, add() RangeError). */
            maxKeys?: number;
            /** 1..4096, default 64; one past it is closed at accept. */
            maxConnections?: number;
        };
    }

    /** ssh-agent (draft-miller-ssh-agent) for ssh-add/ssh: same-euid or root peers only; ed25519, its user certs, sk listed not signed.
     *  FAILURE for confirm, EXTENSION, unknown; msg <= 256 KiB; sign and lock KDF on the worker pool; no agent forwarding anywhere. */
    class SSHAgent implements DynResource {
        /** TypeError: use listen. */
        constructor();
        /** Creates the socket and accepts; holds the event loop until close(). */
        static listen(options: SSHAgentOptions): SSHAgent;
        /** Adds or replaces (same public key) a key; returns its identity. Locked: TypeError; past maxKeys: RangeError. */
        add(key: SSHAgentKey): SSHAgentIdentity;
        /** Removes by public key line; returns whether held. Locked or bad line: TypeError. */
        remove(publicKey: string): boolean;
        /** Drops every key; locked: TypeError. */
        removeAll(): void;
        /** Identities in add order (the wire order); [] while locked. */
        list(): SSHAgentIdentity[];
        readonly path: string | null;
        /** LOCK/UNLOCK over the wire (PBKDF2-HMAC-SHA512 x50000, 16-byte salt); locked lists and signs nothing. */
        readonly locked: boolean;
        /** refused: failed SIGNs and wrong UNLOCKs; offloaded: signatures and KDF runs done on the worker pool. */
        readonly stats: { keys: number; connections: number; accepted: number; refusedConnections: number; requests: number; signatures: number; refused: number; offloaded: number };
        close(): void;
        dispose(): void;
        readonly closed: boolean;
        readonly [Symbol.dispose]: () => void;
    }
}

/** dyna:sftp — SFTP version 3 (draft-ietf-secsh-filexfer-02, as OpenSSH speaks it) over dyna:ssh, client and server. Both sides frame
 *  packets in C on the SSH channel; no JS runs per packet except the server's optional allow hook. Packets are capped at 256 KiB
 *  (a bigger or malformed one from the peer ends the session); a read asks for at most 261120 bytes. Paths are UTF-8 strings; SYMLINK
 *  carries the link target first and the link path second, the order OpenSSH uses (the draft lists them the other way round), so
 *  both sides interoperate with OpenSSH. No extensions are advertised or used (v3 RENAME refuses an existing target). Errors are
 *  Error objects whose message starts "SFTP: ", with `code` (the status name: NO_SUCH_FILE, PERMISSION_DENIED, FAILURE, EOF,
 *  BAD_MESSAGE, NO_CONNECTION, CONNECTION_LOST, OP_UNSUPPORTED) and `status` (its number). */
declare module "dyna:sftp" {
    /** File attributes; a field is present only when the server sent it. isFile / isDirectory / isSymbolicLink come from mode's type bits
     *  (all false without mode). Times are seconds since the epoch. */
    interface SFTPAttrs {
        size?: number;
        uid?: number;
        gid?: number;
        mode?: number;
        atime?: number;
        mtime?: number;
        readonly isFile: boolean;
        readonly isDirectory: boolean;
        readonly isSymbolicLink: boolean;
    }

    /** Attributes to set; a strict bag. uid and gid go together, and so do atime and mtime. size truncates or extends; mode is
     *  masked to 07777 by the server. Integers: size 0..2^53-1, the others 0..2^32-1. */
    interface SFTPSetAttrs {
        size?: number;
        uid?: number;
        gid?: number;
        mode?: number;
        atime?: number;
        mtime?: number;
    }

    interface SFTPEntry {
        filename: string;
        /** The server's ls -l style line (this module's server: mode, links, numeric uid and gid, size, UTC mtime, name). */
        longname: string;
        attrs: SFTPAttrs;
    }

    /** dyna extension: SFTPOpenOptions.dyna, a strict bag. */
    interface SFTPClientMetaOptions {
        /** Requests in flight per streaming file (read-ahead and write-behind), 1..1024; default 64 (the OpenSSH sftp default). */
        maxInflight?: number;
        /** Bytes per READ / WRITE request of the streaming calls, 1024..261120; default 32768. */
        chunkBytes?: number;
    }

    /** Options for SFTPClient.open; a strict bag. */
    interface SFTPOpenOptions {
        dyna?: SFTPClientMetaOptions;
    }

    /** "r" read; "r+" read and write; "w" write, create, truncate; "wx" as w but the file must not exist; "w+" / "wx+" add read;
     *  "a" append (create if missing); "ax" append, must not exist; "a+" / "ax+" add read. */
    type SFTPOpenFlags = "r" | "r+" | "w" | "wx" | "w+" | "wx+" | "a" | "ax" | "a+" | "ax+";

    /** An open remote file. read() and write() are streaming cursors, each starting at 0 and independent of the other:
     *  read() keeps up to maxInflight READ requests of chunkBytes ahead and returns the bytes in file order, filling a short reply by
     *  re-asking for the rest before moving on (so a server that caps its reads below chunkBytes is read correctly); write() splits
     *  the data into chunkBytes WRITE requests and resolves once they are all sent with at most maxInflight unanswered, so a loop of
     *  awaited writes keeps the pipe full; a failed write is reported by the next write() and by close(). One read() and one write()
     *  may be pending at a time. A file collected without close() keeps its server handle until the session ends; one with a read(), write() or close()
     *  pending is a GC root until that settles. */
    interface SFTPFile {
        /** The next bytes in file order (at most chunkBytes), or null at end of file. */
        read(): Promise<Uint8Array | null>;
        /** One READ at `position` for at most `length` bytes (0..261120); null at end of file. The reply may be shorter. */
        pread(position: number, length: number): Promise<Uint8Array | null>;
        write(data: string | ArrayBuffer | ArrayBufferView): Promise<void>;
        /** One WRITE of `data` (at most 261120 bytes) at `position`, resolved when the server acknowledges it. */
        pwrite(position: number, data: string | ArrayBuffer | ArrayBufferView): Promise<void>;
        /** FSTAT / FSETSTAT on the handle. */
        stat(): Promise<SFTPAttrs>;
        setstat(attrs: SFTPSetAttrs): Promise<void>;
        /** Waits for every write to be acknowledged, then CLOSE; rejects with the first write error if one failed. A pending read()
         *  rejects. A second close() rejects. */
        close(): Promise<void>;
        /** Bytes handed to write() so far. */
        readonly writePosition: number;
    }

    interface SFTPDir {
        /** The next batch of entries ("." and ".." included when the server lists them), or null at the end. */
        read(): Promise<SFTPEntry[] | null>;
        close(): Promise<void>;
    }

    /** An SFTP session on an "sftp" subsystem channel of a connected SSHClient (one channel of its dyna.maxChannels). Every method
     *  rejects with NO_CONNECTION once the session is closed. */
    class SFTPClient {
        /** new SFTPClient() is a TypeError: use open. */
        constructor();
        /** Opens the channel, sends INIT and resolves after the server's VERSION (3 or more; older is refused). A closed or
         *  non-SSHClient argument is a TypeError. */
        static open(ssh: import("dyna:ssh").SSHClient, options?: SFTPOpenOptions): Promise<SFTPClient>;
        /** The server's protocol version. */
        readonly version: number;
        /** Requests sent, payload bytes read and written, the most requests ever in flight at once, and those in flight now. */
        readonly stats: { requests: number; bytesRead: number; bytesWritten: number; maxInflight: number; inflight: number };
        stat(path: string): Promise<SFTPAttrs>;
        lstat(path: string): Promise<SFTPAttrs>;
        setstat(path: string, attrs: SFTPSetAttrs): Promise<void>;
        realpath(path: string): Promise<string>;
        readlink(path: string): Promise<string>;
        /** Creates `path` as a symbolic link whose content is `target`. */
        symlink(target: string, path: string): Promise<void>;
        mkdir(path: string, attrs?: SFTPSetAttrs): Promise<void>;
        rmdir(path: string): Promise<void>;
        remove(path: string): Promise<void>;
        /** v3 RENAME: fails when `to` exists. */
        rename(from: string, to: string): Promise<void>;
        /** OPENDIR, READDIR until EOF, CLOSE: every entry. */
        readdir(path: string): Promise<SFTPEntry[]>;
        opendir(path: string): Promise<SFTPDir>;
        /** flags default "r"; attrs apply when the file is created (mode default 0666 before the server's umask). */
        open(path: string, flags?: SFTPOpenFlags, attrs?: SFTPSetAttrs): Promise<SFTPFile>;
        /** The whole file through the pipelined read path. */
        readFile(path: string): Promise<Uint8Array>;
        /** Create or truncate, then the pipelined write path, then CLOSE; rejects with the first failure. */
        writeFile(path: string, data: string | ArrayBuffer | ArrayBufferView, attrs?: SFTPSetAttrs): Promise<void>;
        /** Rejects every pending request (CONNECTION_LOST), closes the channel and resolves once it is closed. */
        close(): Promise<void>;
    }

    /** dyna extension: SFTPServeOptions.dyna, a strict bag (safety caps). */
    interface SFTPServerMetaOptions {
        /** Open file and directory handles per session, 1..4096; default 64. OPEN / OPENDIR past it fail with FAILURE. */
        maxHandles?: number;
        /** Largest READ reply, 1024..261120; default 261120. A bigger request gets a shorter reply (the client re-asks). */
        maxReadBytes?: number;
    }

    /** What the allow hook is asked. `path` is the virtual path ("/" is the root) after resolving symlinks the operation follows
     *  (open, stat, setstat, opendir); for lstat, remove, rmdir, mkdir, readlink, rename and symlink it is the entry itself. `target`
     *  is the rename destination, or the symlink's content as sent. `write` is true for every operation that can change the tree. */
    interface SFTPAllowRequest {
        op: "open" | "stat" | "lstat" | "setstat" | "opendir" | "remove" | "mkdir" | "rmdir" | "readlink" | "rename" | "symlink";
        path: string;
        target?: string;
        user: string;
        write: boolean;
    }

    /** Options for SFTPServer.serve; a strict bag. */
    interface SFTPServeOptions {
        /** The directory the session sees as "/". Every path is resolved component by component from it with openat(O_NOFOLLOW): ".."
         *  never climbs above it, and a symlink is followed by this resolver (at most 40 per path), never by the kernel, with an
         *  absolute target read relative to the root, so neither ".." nor a symlink (in the tree, or made through SYMLINK) reaches a file
         *  outside it. A final component swapped for a symlink between the check and the operation makes the operation fail (O_NOFOLLOW /
         *  AT_SYMLINK_NOFOLLOW). Symlinks are stored with their raw content, so a process outside the session that follows them gets the
         *  kernel's meaning. Only regular files can be opened. */
        root: string;
        /** Default false. true refuses OPEN with any write flag, SETSTAT, FSETSTAT, REMOVE, MKDIR, RMDIR, RENAME and SYMLINK with
         *  PERMISSION_DENIED. FSETSTAT is refused on any handle not opened for writing, whatever readOnly says. */
        readOnly?: boolean;
        /** Called before every path operation (not for READ/WRITE/FSTAT on an open handle, nor REALPATH). true allows; false, a throw,
         *  a rejection or a non-boolean denies with PERMISSION_DENIED. A Promise pauses the session: later requests wait (and the SSH
         *  window stops being granted) until it settles. The approval is bound to the request it was asked for: the operation, resolved path and target are
         *  checked again when the request runs, and if any changed (a rename or symlink swap meanwhile) the request is denied. */
        allow?: (request: SFTPAllowRequest) => boolean | Promise<boolean>;
        dyna?: SFTPServerMetaOptions;
    }

    interface SFTPServeResult {
        requests: number;
        bytesRead: number;
        bytesWritten: number;
        /** Requests refused by readOnly or allow. */
        denied: number;
        /** Why the session ended abnormally (a malformed packet, the connection dying), or null. */
        error: string | null;
    }

    const SFTPServer: {
        /** Takes over an SSHSession, normally of type "subsystem" (command "sftp"); an "exec" or "shell" session is accepted too, for a
         *  custom transport: its stdin no longer delivers data and this server answers
         *  the client. Call it synchronously inside onSession and return its promise from onSession, so the session exits 0 when the
         *  client leaves. The promise resolves (never rejects) when the client sends EOF or the channel closes; every handle is closed
         *  then. A session that already received data, is closing, or is already served, an unknown option key, a bad cap or a root
         *  that cannot be opened as a directory is a TypeError (RangeError for a cap out of range). */
        serve(session: import("dyna:ssh").SSHSession, options: SFTPServeOptions): Promise<SFTPServeResult>;
    };
}

/** dyna:stream — thin sugar over WHATWG streams for bytes bigger than memory: fromBytes/fromFile/stdin are ReadableStreams, toFile/stdout/stderr are WritableStreams,
 *  lines()/ndjson() iterate chunk-safely in C, and toReadableStream/toWritableStream adapt any read(buf)/write(buf) object (subprocess pipes, HTTP getStream bodies).
 *  Compression is CompressionStream/DecompressionStream ("gzip", "deflate", "deflate-raw", "brotli", "zstd", "lz4"); copying is pipeTo/pipeThrough.
 *  v10b-19 BREAKING: pipe(), inflate(), deflate(), fromReadableStream(), fromWritableStream() and the ByteSource/ByteSink return types are gone. */
declare module "dyna:stream" {
    /** The duck-typed pull shape toReadableStream and lines()/ndjson() accept: read(buf) fills up to buf.length bytes and resolves the count, 0 = EOF; close() releases. */
    interface ByteSource {
        read(buf: Uint8Array): Promise<number>;
        close?(): void;
    }
    /** The duck-typed push shape toWritableStream accepts: write(view) resolves the count accepted (fewer is retried); flush() and close() are optional. */
    interface ByteSink {
        write(buf: ByteView): Promise<number>;
        flush?(): Promise<void>;
        close?(): void;
    }
    /** Options for `toFile`. */
    interface ToFileOptions {
        /** Append rather than truncate. */
        append?: boolean;
        /** Write buffer size, clamped to 4 KiB..64 MiB (default 128 KiB); allocated at the first write, so a never-written toFile holds none. */
        bufferSize?: number;
    }
    /** A byte ReadableStream over an in-memory COPY of `b` taken at the call (a string is encoded as UTF-8); chunks of at most 64 KiB; default and BYOB readers. */
    function fromBytes(b: Uint8Array | string): ReadableStream<Uint8Array>;
    /** A byte ReadableStream over a file. The path is checked at the call (a Path or a string without NUL, else TypeError); the file is opened LAZILY at the first read, so a
     *  missing file errors the stream (the read rejects with the errno error, e.g. code ENOENT). Sequential reads of at most 64 KiB, so FIFOs stream too; the fd closes at EOF,
     *  on cancel, on a read error and when the stream is collected. Piped into toFile (no prevent* option, no signal) the copy runs as a C loop with no promise per chunk. */
    function fromFile(path: import("dyna:file").Path | string): ReadableStream<Uint8Array>;
    /** A WritableStream of BufferSource or string (UTF-8) chunks over a file created LAZILY at the first non-empty write (truncate, or `{append: true}`): an unwritable path rejects
     *  that write and errors the stream. Chunks are written in C straight into the write buffer (a chunk of at least a quarter of it that would overflow goes out with the
     *  pending bytes in one writev, uncopied). close() flushes and closes the fd; abort() flushes what is buffered and closes; a write error is sticky. */
    function toFile(path: import("dyna:file").Path | string, opts?: ToFileOptions): WritableStream<ArrayBufferView | ArrayBuffer | string>;
    /** Options for `lines`. */
    interface LinesOptions {
        /** Only "utf-8" (the default) is supported; anything else is a TypeError; a value whose toString throws propagates that error. */
        encoding?: string;
        /** Yield string[] batches (one per source read) instead of single lines. */
        batch?: boolean;
    }
    /** Async iterable of lines, safe across ANY chunk boundary; strips CRLF, keeps empty lines, delivers an unterminated last line; `break` cancels the source.
     *  src is a ReadableStream of BufferSource/string chunks (locked by a reader that is released at the end; a locked stream is a TypeError) or an object with read(buf), which is read through toReadableStream (reads of at most 64 KiB, close() at EOF and on return(), a count outside [0, buf.length] or a non-number rejects next()); a live iterator holds no read buffer of its own.
     *  One C splitter (SIMD newline scan that resumes where the last chunk stopped, ASCII lines copied straight into 8-bit strings) serves both shapes. Per line, an already-buffered line
     *  is returned as an already-resolved promise and the source is read only when no complete line is buffered; next() while a read is pending is a TypeError.
     *  With `{batch: true}` each step yields string[]: every complete line from one source read (never empty, at most 65536), so a loop pays one promise per batch, not per line. */
    function lines(src: ByteSource | ReadableStream<ArrayBufferView | ArrayBuffer | string>, opts?: LinesOptions): AsyncIterableIterator<string>;
    function lines(src: ByteSource | ReadableStream<ArrayBufferView | ArrayBuffer | string>, opts: LinesOptions & { batch: true }): AsyncIterableIterator<string[]>;
    /** Async iterable of NDJSON values (sources and batching as for `lines`); blank lines are skipped; a malformed line rejects with its 1-based line number and iteration can resume after it.
     *  In batch mode the values before a malformed line arrive first as their own batch and the next step rejects with the error. */
    function ndjson(src: ByteSource | ReadableStream<ArrayBufferView | ArrayBuffer | string>, opts?: { batch?: false }): AsyncIterableIterator<unknown>;
    function ndjson(src: ByteSource | ReadableStream<ArrayBufferView | ArrayBuffer | string>, opts: { batch: true }): AsyncIterableIterator<unknown[]>;
    /** Options for `toReadableStream`. */
    interface ToReadableStreamOptions {
        /** Bytes per pull for a default reader (the buffer handed to src.read; a short read of under half of it is copied out at its length, a longer one keeps the buffer), 1..2^30; default 64 KiB. A BYOB reader's view is passed to src.read as is, whatever its size. */
        chunkSize?: number;
        /** Readable-side highWaterMark in bytes; default 0 (pull only on demand). */
        highWaterMark?: number;
    }
    /** Options for `toWritableStream`. */
    interface ToWritableStreamOptions {
        /** Chunks queued before backpressure; default 1. */
        highWaterMark?: number;
    }
    /** A WHATWG byte ReadableStream over a read(buf) object (a subprocess pipe, an HTTP getStream body, any duck source). Each pull calls src.read(view) on the BYOB view itself (no copy);
     *  read() resolving 0 closes the stream and src; cancel(), a rejected read() and a count outside [0, view.length] close src (the last two error the stream).
     *  Piped into a toWritableStream stream with no prevent* option and no signal, while both are idle, the pipe skips the per-chunk WHATWG machinery and runs a C pipe
     *  engine (128 KiB reads, short writes retried). At EOF src is closed, then the WritableStream closes (flush, then close() of the sink); on error both sides are closed and both streams error with it. */
    function toReadableStream(src: ByteSource, opts?: ToReadableStreamOptions): ReadableStream<Uint8Array>;
    /** A WHATWG WritableStream over a write(buf) object. Chunks are BufferSources or strings (UTF-8); a partial write() loops until the chunk is accepted (0 or more than offered is a TypeError).
     *  close() awaits sink.flush() when present, then sink.close(); abort() closes the sink. */
    function toWritableStream(sink: ByteSink, opts?: ToWritableStreamOptions): WritableStream<ArrayBufferView | ArrayBuffer | string>;
    /** This process's stdin as a byte ReadableStream: the SAME stream on every call until it closes, errors or is cancelled, then a fresh one (cancel settles a pending read done and leaves fd 0 open).
     *  A pipe, tty or socket is read when the event loop reports it readable (the JS thread never blocks, and a pending read keeps the loop alive); a regular file or a non-tty character device such as /dev/null is read directly,
     *  in C straight into the BYOB view or a fresh chunk (a chunk that fills under half of 64 KiB is copied to its exact size, otherwise it keeps the buffer, whose unread tail is zeroed).
     *  The fd is never switched to O_NONBLOCK (its open file description is shared with the parent). */
    function stdin(): ReadableStream<Uint8Array>;
    /** A new WritableStream over this process's stdout per call. Small writes are coalesced, like bun (owner decision D5):
     *  - A chunk under 16 KiB is copied into one process-wide buffer for fd 1 (at most 64 KiB, shared by every stdout() stream and every thread) and its write
     *    resolves at once, before the bytes reach the fd. It costs no allocation; an unused buffer costs no memory.
     *  - The buffer is written out when the next chunk would overflow it; when the event loop turns or goes idle (before it polls, so a timer, an awaited read
     *    or the end of the script flushes it); before a chunk of 16 KiB or more, which goes out with the buffered bytes in one writev, uncopied; on writer.close();
     *    before console.*, print, std.puts/printf/printObject, any std.out/std.err method, os.write/os.close/os.dup2 on fd 1 or 2, dyna:term output and dyna:log
     *    stderr output; before os.exec and dyna:sys Exec/Spawn fork; before an uncaught error or unhandled rejection is printed; at std.exit; and at process exit.
     *    A --timeout-ms hard stop (exit 113) flushes only when no other thread holds the buffer. Code that writes fd 1 or 2 by other means may overtake buffered bytes.
     *  - Order: bytes reach the fd in call order across stdout(), stderr(), console and std.out/std.err (C stdio output buffered by std.out is flushed before a
     *    chunk is buffered). Writing to stderr first flushes stdout and writing to stdout first flushes stderr, so the two never reorder on a shared terminal or a 2>&1 pipe.
     *    console.log itself stays unbuffered (written and flushed per call).
     *  - Errors stick to the fd for the rest of the process: a write error found by a flush (EPIPE once the reader is gone, ENOSPC, ...) rejects the next write,
     *    writer.close() and any later stdout() stream with TypeError "stdout: write failed: <strerror>", and the stream errors with it. An error that no write or
     *    close() reported is printed at exit as "TypeError: stdout: write failed: <strerror>" and the exit status becomes 1 (std.exit(n) keeps a non-zero n), as an
     *    uncaught rejected write did before D5. The buffered bytes of a failed flush are dropped.
     *  - On a non-blocking fd (shared with another process), EAGAIN while flushing the buffer waits with poll(POLLOUT); EAGAIN on a large chunk parks the rest of
     *    that chunk, copied, until the fd is writable, and the write settles when it is all written.
     *  close() leaves fd 1 open. */
    function stdout(): WritableStream<ArrayBufferView | ArrayBuffer | string>;
    /** stderr (fd 2) as a WritableStream, with stdout()'s rules: its own coalescing buffer and sticky error ("stderr: write failed: ..."); a stderr write flushes stdout first. */
    function stderr(): WritableStream<ArrayBufferView | ArrayBuffer | string>;
}


/** dyna:structures — heaps, LRU, tries, B-trees, sorted maps, segment trees, bitsets, sketches, range sets, roaring bitmaps, k-d trees, BM25 indexes.
 *  Each persists via serialize()/deserialize(); bytes are untrusted (lengths validated before allocating; at most 512 MiB + 64x the record's payload per record).
 *  FILES  x.save(path, opts?) writes one file and Class.load(path, opts?) reads it back; both are synchronous. The format is dyna persist v1: a superblock, two header slots A/B (each with its own crc32c; load uses the intact one with the higher generation), and sections with one crc32c each. Large sections are 16 KiB aligned. BloomFilter, CountMinSketch and HyperLogLog store their counter arrays raw, written straight from memory with writev. Every other class stores its serialize() record as one section. The writer's buffers are reused per thread.
 *  save is atomic: it writes a temp file in the same directory, fsyncs it, renames it over `path`, then fsyncs the directory. A crash leaves the old file or the new one; a stray "<path>.tmp.<pid>.<n>" may remain.
 *  With fsync (the default) a returned save is durable across power loss: on macOS plain fsync() leaves data in the drive's cache, so every sync there is fcntl(F_FULLFSYNC), falling back to fsync() only where the filesystem refuses it. That costs milliseconds per save on Apple SSDs; pass fsync: false when atomicity alone is enough.
 *  An incremental save never overwrites existing data. It appends the changed 16 KiB chunks and an overlay table, fsyncs, then rewrites the OLDER header slot and fsyncs again. A crash or power loss at any step loads as the previous or the new state.
 *  It falls back to a full save when: the instance did not save this file or load it last (lineage/generation mismatch); the first save after a load; the layout changed; or the appended space exceeds the base size (compaction).
 *  load checks before trusting anything: magic, version, byte order, superblock/slot/overlay/section checksums, every offset and length, then the structure's own invariants. A file of another class is EPERSIST_TYPE.
 *  mmap: true maps the file privately. The raw classes use their arrays in place: writes stay copy-on-write and the file is never modified, and the instance keeps the mapping and one descriptor until it is collected. Another process truncating or rewriting the file in place while it is mapped raises SIGBUS; dyna's own saves never do either (full saves rename, incremental saves only append). The default is a private copy.
 *  Errors are Error objects with .code. EPERSIST_IO also carries .errno. The other codes: EPERSIST_MAGIC, EPERSIST_VERSION, EPERSIST_ENDIAN, EPERSIST_TRUNCATED, EPERSIST_CORRUPT, EPERSIST_CHECKSUM, EPERSIST_TYPE, EPERSIST_MISSING, EPERSIST_TOO_LARGE, EPERSIST_NOMEM. A record that fails to decode (reachable with verify: false) throws EPERSIST_CHECKSUM or EPERSIST_CORRUPT; Heap.load rebuilds through push, so its compare function runs during load; an error it throws comes through unchanged (no .code added). A path that is not a dyna:file Path, or a non-boolean option, is a TypeError.
 *  Stored arrays are written index by index with holes as undefined; serialize() throws RangeError for an array with more than 2^24 holes beyond its real elements. */
declare module "dyna:structures" {
    /** Options for `save(path, opts)` on every dyna:structures class. */
    interface PersistSaveOptions {
        /** Durably sync the file, then its directory (default true; F_FULLFSYNC on macOS, see FILES); false trades durability for speed, atomicity is kept. */
        fsync?: boolean;
        /** Default false. BloomFilter, CountMinSketch and HyperLogLog only: append just the changed 16 KiB chunks and commit through the older header slot; other classes, and every case listed under FILES, write a full save. */
        incremental?: boolean;
    }

    /** Options for `Class.load(path, opts)`. */
    interface PersistLoadOptions {
        /** Map the file (MAP_PRIVATE) instead of reading it (default false). BloomFilter, CountMinSketch and HyperLogLog then use their arrays in place, copy-on-write; see FILES for the SIGBUS hazard. */
        mmap?: boolean;
        /** Check every section checksum (default true); false skips only those, never the header checksum or the bounds checks. */
        verify?: boolean;
    }

    /** What `save` reports. */
    interface PersistSaveResult {
        /** The path argument, as passed. */
        path: import("dyna:file").Path;
        /** The file's logical size after the save. */
        bytes: number;
        /** Bytes this save wrote (smaller than `bytes` for an incremental save). */
        written: number;
        /** True when the save appended chunks instead of rewriting the file. */
        incremental: boolean;
    }

    /** Capacity-bounded string-to-value cache with LRU eviction. */
    class LRU<V = unknown> {
        /** LRU cache of `capacity` entries; `ttlMs` expires entries, `onEvict(key, value)` observes evictions. */
        constructor(capacity: number, opts?: { ttlMs?: number; onEvict?: (key: string, value: V) => void });
        /** Rebuilds from serialize() bytes; T flows through, so no cast is needed. */
        static deserialize<T>(this: new (capacity: number, opts?: { ttlMs?: number; onEvict?: (key: string, value: T) => void }) => LRU<T>, bytes: Uint8Array | ArrayBuffer): LRU<T>;
        /** Reads a file written by save(); see the module notes on FILES for options and errors. */
        static load<T>(this: new (capacity: number, opts?: { ttlMs?: number; onEvict?: (key: string, value: T) => void }) => LRU<T>, path: import("dyna:file").Path, opts?: PersistLoadOptions): LRU<T>;
        /** The value and moves the entry to MRU. An expired entry is dropped (counted in `expired`) before the miss. */
        get(key: string): V | undefined;
        /** Inserts or updates, marking the entry MRU; past capacity the LRU entry is evicted. */
        put(key: string, value: V): this;
        /** Alias for `put`. */
        set(key: string, value: V): this;
        /** Stores an entry with its own lifetime in ms. */
        setWithTTL(key: string, value: V, ms: number): this;
        /** Presence without touching recency or counting a hit; an expired key reads absent. */
        has(key: string): boolean;
        /** Removes the entry if present. */
        delete(key: string): boolean;
        /** Reclaims every expired entry now; returns the count removed. */
        purgeExpired(): number;
        /** The number of entries. */
        readonly size: number;
        /** The capacity bound. */
        readonly capacity: number;
        /** `{ hits, misses, evictions, expired, size, capacity }`. */
        readonly stats: { hits: number; misses: number; evictions: number; expired: number; size: number; capacity: number };
        /** The contents as a compact type-tagged record. */
        serialize(): Uint8Array;
        /** Writes the structure to `path` atomically; see the module notes on FILES. */
        save(path: import("dyna:file").Path, opts?: PersistSaveOptions): PersistSaveResult;
    }

    /** Binary heap ordered by a JS comparator or natural number order. */
    class Heap<V = number> {
        /** Creates a binary heap. The comparator must not push/pop its own heap (reentrancy throws `TypeError`). */
        constructor(comparator?: (a: V, b: V) => number);
        /** Rebuilds from serialize() bytes with the comparator; T flows from it. */
        static deserialize<T>(bytes: Uint8Array | ArrayBuffer, cmp: (a: T, b: T) => number): Heap<T>;
        static deserialize<T>(this: new (comparator?: (a: T, b: T) => number) => Heap<T>, bytes: Uint8Array | ArrayBuffer): Heap<T>;
        /** Reads a file written by save(); `compare` is the comparator (natural order when omitted). See the module notes on FILES. */
        static load<T>(path: import("dyna:file").Path, opts?: PersistLoadOptions & { compare?: (a: T, b: T) => number }): Heap<T>;
        /** Insert and sift; returns the new size. */
        push(v: V): number;
        /** Removes and returns the root; `undefined` when empty. */
        pop(): V | undefined;
        /** The root without removal; never calls the comparator. */
        peek(): V | undefined;
        /** The element count. Getter; `Heap.length` is the same getter. */
        readonly size: number;
        readonly length: number;
        /** The contents as a compact type-tagged record. A comparator is not data, so the record cannot carry it. */
        serialize(): Uint8Array;
        /** Writes the structure to `path` atomically; see the module notes on FILES. */
        save(path: import("dyna:file").Path, opts?: PersistSaveOptions): PersistSaveResult;
    }

    /** Atkinson/Sack/Santoro/Strothotte min-max heap. */
    class MinMaxHeap<V = number> {
        /** Creates an empty min-max heap. */
        constructor();
        /** Rebuilds a min-max heap from its record. */
        static deserialize<T>(this: new () => MinMaxHeap<T>, bytes: Uint8Array | ArrayBuffer): MinMaxHeap<T>;
        /** Reads a file written by save(); see the module notes on FILES for options and errors. */
        static load<T>(this: new () => MinMaxHeap<T>, path: import("dyna:file").Path, opts?: PersistLoadOptions): MinMaxHeap<T>;
        /** Inserts a value with a numeric priority. */
        push(priority: number, value?: V): this;
        /** Removes and returns the minimum's stored value. */
        popMin(): V | undefined;
        /** Removes and returns the maximum's stored value. */
        popMax(): V | undefined;
        /** The minimum without removal. */
        peekMin(): V | undefined;
        /** The maximum without removal. */
        peekMax(): V | undefined;
        /** The element count. */
        readonly size: number;
        /** The contents as a compact type-tagged record. */
        serialize(): Uint8Array;
        /** Writes the structure to `path` atomically; see the module notes on FILES. */
        save(path: import("dyna:file").Path, opts?: PersistSaveOptions): PersistSaveResult;
    }

    /** Set of numbers in sorted order (the same ordered-tree core as BTree). */
    class SortedSet {
        /** Creates an empty set. NaN keys are rejected on insert (`RangeError`). */
        constructor();
        /** Rebuilds a set from its record. */
        static deserialize(bytes: Uint8Array | ArrayBuffer): SortedSet;
        /** Reads a file written by save(); see the module notes on FILES for options and errors. */
        static load(path: import("dyna:file").Path, opts?: PersistLoadOptions): SortedSet;
        /** Inserts the key if absent; duplicates are no-ops. */
        add(x: number): this;
        /** Whether the key is present. */
        has(x: number): boolean;
        /** Removes the key, reporting whether it was present. */
        delete(x: number): boolean;
        /** The smallest key. */
        first(): number | undefined;
        /** The largest key. */
        last(): number | undefined;
        /** The largest element <= x (ceil: the smallest >= x). */
        floor(x: number): number | undefined;
        /** The smallest key ≥ `x`; `undefined` if none or `x` is NaN. */
        ceil(x: number): number | undefined;
        /** Ascending keys in [lo, hi]. */
        rangeQuery(lo: number, hi: number): number[];
        /** All keys ascending. */
        toArray(): number[];
        /** The key count. */
        readonly size: number;
        [Symbol.iterator](): Iterator<number>;
        /** The contents as a compact type-tagged record. */
        serialize(): Uint8Array;
        /** Writes the structure to `path` atomically; see the module notes on FILES. */
        save(path: import("dyna:file").Path, opts?: PersistSaveOptions): PersistSaveResult;
    }

    /** Sorted map from numeric key to value (same core as BTree, but a DISTINCT serialize format and no [Symbol.iterator]). */
    class SortedMap<V = unknown> {
        /** Creates an empty map. NaN keys are rejected on insert (`RangeError`). */
        constructor();
        /** Rebuilds a map from its record. */
        static deserialize<T>(this: new () => SortedMap<T>, bytes: Uint8Array | ArrayBuffer): SortedMap<T>;
        /** Reads a file written by save(); see the module notes on FILES for options and errors. */
        static load<T>(this: new () => SortedMap<T>, path: import("dyna:file").Path, opts?: PersistLoadOptions): SortedMap<T>;
        /** Inserts or replaces. A NaN key throws `RangeError`. */
        set(k: number, v: V): this;
        /** The value; `undefined` for absent or NaN keys. */
        get(k: number): V | undefined;
        /** Whether the key is present. */
        has(k: number): boolean;
        /** Removes the key, reporting whether it was present. */
        delete(k: number): boolean;
        /** The smallest key. */
        firstKey(): number | undefined;
        /** The largest key. */
        lastKey(): number | undefined;
        /** The largest key <= k (ceilKey: the smallest >= k). */
        floorKey(k: number): number | undefined;
        /** The smallest key ≥ `k`; `undefined` if none or `k` is NaN. */
        ceilKey(k: number): number | undefined;
        /** Ascending `[k, v]` pairs in `[lo, hi]`. */
        rangeQuery(lo: number, hi: number): [number, V][];
        /** Ascending keys. */
        keys(): number[];
        /** The key count. */
        readonly size: number;
        /** The contents as a compact type-tagged record. */
        serialize(): Uint8Array;
        /** Writes the structure to `path` atomically; see the module notes on FILES. */
        save(path: import("dyna:file").Path, opts?: PersistSaveOptions): PersistSaveResult;
    }

    /** Ordered-map options: `keys` is "number" (default; NaN refused, -0 stored as +0) or "string" (UTF-8 byte order). */
    interface OrderedTreeOptions {
        /** The key kind. */
        keys?: "number" | "string";
    }

    /** Ordered map; O(log n) updates, in-order iteration. The name is historical: storage is the
     * counted B-tree shared with WBTree and SkipList (31 keys per node; each node is its own allocation
     * addressed by a u32 slot table, leaves without child links or counts; see WBTree for the D4 layout), not a red-black tree.
     * Iteration order, answers and save() records are byte-identical to the old red-black tree. Up to
     * 128 entries the map is one sorted array grown 2, 4, ... 128 (no node pool), then it is
     * bulk-loaded into the B-tree. Measured against the red-black tree: at N=16..512 every op is
     * 1.0-1.9x faster at 37-60 B/entry (was 61-85); at N >= 1024 set 1.25-2.8x, get 1.15-1.7x,
     * delete 1.3-2.4x, floorKey/ceilKey 1.25-1.9x faster at 37-47 B/entry (was 60); repeated
     * build/drop runs plateau. */
    class RBTree<K extends number | string = number, V = unknown> {
        /** Creates an empty tree. */
        constructor(options?: OrderedTreeOptions);
        /** Inserts or replaces. */
        set(k: K, v: V): this;
        /** The value, or `undefined`. */
        get(k: K): V | undefined;
        /** Whether the key is present. */
        has(k: K): boolean;
        /** Removes the key, reporting whether it was present. */
        delete(k: K): boolean;
        /** Ascending keys. */
        keys(): K[];
        /** Values in key order. */
        values(): V[];
        /** Ascending `[key, value]` pairs. */
        entries(): [K, V][];
        [Symbol.iterator](): Iterator<[K, V]>;
        /** The smallest key. */
        firstKey(): K | undefined;
        /** The largest key. */
        lastKey(): K | undefined;
        /** The largest key <= `k`. */
        floorKey(k: K): K | undefined;
        /** The smallest key >= `k`. */
        ceilKey(k: K): K | undefined;
        /** The key count. */
        readonly size: number;
        /** Writes the structure to `path` atomically; see the module notes on FILES. */
        save(path: import("dyna:file").Path, opts?: PersistSaveOptions): PersistSaveResult;
        /** Reads a file written by save(); see the module notes on FILES for options and errors. */
        static load(path: import("dyna:file").Path, opts?: PersistLoadOptions): RBTree;
    }

    /** Ordered map with O(log n) rank and select. The name is historical: storage is the counted
     * B-tree shared with RBTree and SkipList, not a weight-balanced tree. Iteration order, answers
     * and save() records are byte-identical to the old weight-balanced tree. Up to 128 entries the
     * map is one sorted array (see RBTree). Layout (D4, 11-21): one allocation per node (no 32-node chunk slack);
     * unused number-key slots hold +Infinity so get/rank/floorKey/ceilKey count all 31 slots without a
     * data-dependent branch (set/delete keep the branchy scan, whose speculation wins on writes); rank adds the
     * child counts left of the descent with one masked NEON/SSE2 sum; a full leaf receiving a key past its last
     * (or before its first) key on the tree's right (left) edge keeps 30 keys and starts a new edge leaf, so
     * ascending/descending fills stay near 30 of 31 slots. Measured against the weight-balanced tree (best of 5
     * interleaved, number keys, random order): every op at N=16..1M is 1.05-2.4x faster (rank 1.25x at 16,
     * 1.31x at 256, 1.19x at 1024, 1.55x at 64K, 2.25x at 1M); memory 36-49 B/entry at N=128..64K random and
     * 28-49 ascending (was 48-51), 37.3 at 1M (was 48.3), never above the old tree at any swept N; repeated
     * build/drop runs plateau. Against the pre-D4 B-tree: every op 1.0-1.4x faster except iterate at 1M (0.97x),
     * and the chunk sawtooth (62 B/entry at 768, 70 ascending) is gone. */
    class WBTree<K extends number | string = number, V = unknown> {
        /** Creates an empty tree. */
        constructor(options?: OrderedTreeOptions);
        /** Inserts or replaces. */
        set(k: K, v: V): this;
        /** The value, or `undefined`. */
        get(k: K): V | undefined;
        /** Whether the key is present. */
        has(k: K): boolean;
        /** Removes the key, reporting whether it was present. */
        delete(k: K): boolean;
        /** Ascending keys. */
        keys(): K[];
        /** Values in key order. */
        values(): V[];
        /** Ascending `[key, value]` pairs. */
        entries(): [K, V][];
        [Symbol.iterator](): Iterator<[K, V]>;
        /** The smallest key. */
        firstKey(): K | undefined;
        /** The largest key. */
        lastKey(): K | undefined;
        /** The largest key <= `k`. */
        floorKey(k: K): K | undefined;
        /** The smallest key >= `k`. */
        ceilKey(k: K): K | undefined;
        /** The key count. */
        readonly size: number;
        /** The number of keys below `k`. */
        rank(k: K): number;
        /** The `[key, value]` at sorted position `i` (negative counts from the end). */
        at(i: number): [K, V] | undefined;
        /** Writes the structure to `path` atomically; see the module notes on FILES. */
        save(path: import("dyna:file").Path, opts?: PersistSaveOptions): PersistSaveResult;
        /** Reads a file written by save(); see the module notes on FILES for options and errors. */
        static load(path: import("dyna:file").Path, opts?: PersistLoadOptions): WBTree;
    }

    /** Ordered map with O(log n) rank and select. The name is historical: storage is a counted B-tree
     * (31 keys per node, per-child subtree counts in inner nodes only, each node its own allocation; see WBTree), not a skip list.
     * Iteration order, rank/at answers and save() records are byte-identical to the old skip list.
     * Measured against it at N=128..100K: set 1.5-1.8x, get 1.5-2.1x, delete 1.3-1.6x,
     * rank 1.5-2.1x, at 1.3-1.9x faster; 37-47 B/entry at N >= 1024 (was 66); up to 128 entries it
     * is one sorted array (see RBTree): 60 B/entry at 16 (was 1147 on the first B-tree), 37 at 128
     * (was 73 on the skip list). Splitting leaves into their own pool cost 3-8% CPU on set, get and
     * rank at N=512..100K against the single-pool B-tree for 20% less memory (46.9 -> 37.5 B/entry
     * at 100K). floorKey/ceilKey take one descent. */
    class SkipList<K extends number | string = number, V = unknown> {
        /** Creates an empty list. */
        constructor(options?: OrderedTreeOptions);
        /** Inserts or replaces. */
        set(k: K, v: V): this;
        /** The value, or `undefined`. */
        get(k: K): V | undefined;
        /** Whether the key is present. */
        has(k: K): boolean;
        /** Removes the key, reporting whether it was present. */
        delete(k: K): boolean;
        /** Ascending keys. */
        keys(): K[];
        /** Values in key order. */
        values(): V[];
        /** Ascending `[key, value]` pairs. */
        entries(): [K, V][];
        [Symbol.iterator](): Iterator<[K, V]>;
        /** The smallest key. */
        firstKey(): K | undefined;
        /** The largest key. */
        lastKey(): K | undefined;
        /** The largest key <= `k`. */
        floorKey(k: K): K | undefined;
        /** The smallest key >= `k`. */
        ceilKey(k: K): K | undefined;
        /** The key count. */
        readonly size: number;
        /** The number of keys below `k`. */
        rank(k: K): number;
        /** The `[key, value]` at sorted position `i` (negative counts from the end). */
        at(i: number): [K, V] | undefined;
        /** Writes the structure to `path` atomically; see the module notes on FILES. */
        save(path: import("dyna:file").Path, opts?: PersistSaveOptions): PersistSaveResult;
        /** Reads a file written by save(); see the module notes on FILES for options and errors. */
        static load(path: import("dyna:file").Path, opts?: PersistLoadOptions): SkipList;
    }

    /** Sparse array over 64-bit indices (a radix tree with 3 mark bits per slot); indices above 2^53 are BigInts. */
    class XArray<V = unknown> {
        /** Creates an empty array. */
        constructor();
        /** Stores `v` at index `i` (a safe integer or a BigInt below 2^64). */
        set(i: number | bigint, v: V): this;
        /** The value at `i`, or `undefined`. */
        get(i: number | bigint): V | undefined;
        /** Whether index `i` holds a value. */
        has(i: number | bigint): boolean;
        /** Removes index `i`, reporting whether it held a value. */
        delete(i: number | bigint): boolean;
        /** The first `[index, value]` with `from <= index <= max`. */
        next(from?: number | bigint, max?: number | bigint): [number | bigint, V] | undefined;
        /** The first `[index, value]` in `[from, max]` carrying `mark` (0..2). */
        nextMarked(mark: number, from?: number | bigint, max?: number | bigint): [number | bigint, V] | undefined;
        /** Sets (or with `on` false clears) `mark` on an occupied slot; false when the slot is empty. */
        mark(i: number | bigint, mark: number, on?: boolean): boolean;
        /** Whether slot `i` carries `mark`. */
        isMarked(i: number | bigint, mark: number): boolean;
        /** `[index, value]` pairs in index order within `[from, max]`. */
        entries(from?: number | bigint, max?: number | bigint): [number | bigint, V][];
        [Symbol.iterator](): Iterator<[number | bigint, V]>;
        /** The occupied slot count. */
        readonly size: number;
        /** Writes the structure to `path` atomically; see the module notes on FILES. */
        save(path: import("dyna:file").Path, opts?: PersistSaveOptions): PersistSaveResult;
        /** Reads a file written by save(); see the module notes on FILES for options and errors. */
        static load(path: import("dyna:file").Path, opts?: PersistLoadOptions): XArray;
    }

    /** Path-compressed binary trie over string, byte or CIDR keys; a trie holds one key space at a time. serialize/save
     * write record version 2 (raw keys in walk order, loaded with a path cursor in about linear time); version 1
     * records still load, and a build that predates version 2 refuses it as a truncated DYNS record.
     * Storage: 16-byte nodes with u32 children in 1024-entry chunks (the first chunk grows 8, 16, .. 1024 so small tries stay
     * small and repeated build/drop runs plateau), plus one value slot per entry holding keys up to 16 bytes inline (longer keys
     * own one heap block). Lookups test only the branch bits and compare the full key once at the end; walk order, records and
     * longestPrefix results are unchanged. About 60-68 B/entry for short string keys (was 116), 112 for 35-byte keys (was 173). */
    class PatriciaTrie<V = unknown> {
        /** Creates a trie over UTF-8 strings (default) or `Uint8Array` keys (`keys: "bytes"`). */
        constructor(options?: { keys?: "string" | "bytes" });
        /** Inserts or replaces. */
        set(k: string | Uint8Array, v: V): this;
        /** The value, or `undefined`. */
        get(k: string | Uint8Array): V | undefined;
        /** Whether the key is present. */
        has(k: string | Uint8Array): boolean;
        /** Removes the key, reporting whether it was present. */
        delete(k: string | Uint8Array): boolean;
        /** The `[key, value]` of the longest stored key that prefixes `k`. */
        longestPrefix(k: string | Uint8Array): [string | Uint8Array, V] | undefined;
        /** Every `[key, value]` whose key starts with `prefix`, in byte order. */
        withPrefix(prefix?: string | Uint8Array): [string | Uint8Array, V][];
        /** Stores `v` under an IPv4/IPv6 block such as "10.0.0.0/8" (no length: a host route). */
        addCIDR(cidr: string, v: V): this;
        /** The value of the most specific block containing `ip`; IPv4 and IPv6 never match each other. */
        matchIP(ip: string): V | undefined;
        /** Removes a block, reporting whether it was present. */
        deleteCIDR(cidr: string): boolean;
        /** The key count. */
        readonly size: number;
        /** Writes the structure to `path` atomically; see the module notes on FILES. */
        save(path: import("dyna:file").Path, opts?: PersistSaveOptions): PersistSaveResult;
        /** Reads a file written by save(); see the module notes on FILES for options and errors. */
        static load(path: import("dyna:file").Path, opts?: PersistLoadOptions): PatriciaTrie;
    }

    /** Immutable graph in compressed sparse row form, nodes 0..n-1. */
    class CSRGraph {
        /** Builds from `[src, dst, weight = 1]` edges over 0..2^26 nodes; undirected (`directed: false`) stores each edge both ways. */
        constructor(nodes: number, edges: [number, number, number?][], options?: { directed?: boolean });
        /** The node count. */
        readonly nodes: number;
        /** The stored edge count (twice the input for undirected graphs). */
        readonly edgeCount: number;
        /** Out-neighbours of `v`. */
        neighbors(v: number): Uint32Array;
        /** Out-edge weights of `v`, aligned with `neighbors(v)`. */
        weights(v: number): Float64Array;
        /** The out-degree of `v`. */
        degree(v: number): number;
        /** Hop counts from `src`; -1 where unreachable. */
        bfs(src: number): Int32Array;
        /** Shortest distances and predecessors from `src` (-1: none); a negative weight throws `RangeError`. */
        dijkstra(src: number): { dist: Float64Array; prev: Int32Array };
        /** PageRank by power iteration; dangling mass is spread evenly (defaults 0.85, 100, 1e-10). */
        pagerank(options?: { damping?: number; iterations?: number; tolerance?: number }): Float64Array;
        /** Weakly connected components: a label per node and their count. */
        components(): { count: number; labels: Uint32Array };
        /** Writes the structure to `path` atomically; see the module notes on FILES. */
        save(path: import("dyna:file").Path, opts?: PersistSaveOptions): PersistSaveResult;
        /** Reads a file written by save(); see the module notes on FILES for options and errors. */
        static load(path: import("dyna:file").Path, opts?: PersistLoadOptions): CSRGraph;
    }

    /** R*-tree of axis-aligned boxes with numeric ids, 1..8 dimensions. */
    class RStarTree {
        /** Creates a tree of `dims` dimensions (default 2). */
        constructor(dims?: number);
        /** Adds box `[lo, hi]` with id `id` (a non-negative safe integer). */
        insert(lo: number[], hi: number[], id: number): this;
        /** Removes the entry with exactly this box and id, reporting whether it existed. */
        remove(lo: number[], hi: number[], id: number): boolean;
        /** Ids of every box intersecting `[lo, hi]` (edges inclusive). */
        search(lo: number[], hi: number[]): number[];
        /** The `k` (1..100000) boxes nearest `point`, closest first, with Euclidean box distance. */
        nearest(point: number[], k?: number): { id: number; distance: number }[];
        /** The entry count. */
        readonly size: number;
        /** The tree height (1 for a single leaf). */
        readonly height: number;
        /** The dimension count. */
        readonly dims: number;
        /** Writes the structure to `path` atomically; see the module notes on FILES. */
        save(path: import("dyna:file").Path, opts?: PersistSaveOptions): PersistSaveResult;
        /** Reads a file written by save(); see the module notes on FILES for options and errors. The tree is bulk-built (STR packing), so its height, node layout, search() order and serialize() bytes may differ from the saved tree; the entry set and every query result set are the same. */
        static load(path: import("dyna:file").Path, opts?: PersistLoadOptions): RStarTree;
    }

    /** Suffix array (SA-IS) over a string or bytes; string positions and lengths are UTF-16 units, order is UTF-8 byte order. */
    class SuffixArray {
        /** Builds the array; text over 4 GiB of UTF-8 throws `RangeError`. `cache` (default off): `bucket` (or `cache: true`)
         * builds a table of where each first byte (texts of 1024+ bytes, 1 KiB) or first two bytes (2^18+ bytes, 256 KiB)
         * start in the array, so count/find of patterns at least that long search one block: about 1.3-1.5x faster below
         * 2^18 bytes, 2x at 2^18, 3x at 10 MB. Results never change; a loaded or deserialized array starts without it.
         * Unknown option keys throw TypeError. */
        constructor(text: string | Uint8Array, options?: { cache?: boolean | { bucket?: boolean } });
        /** Suffix start positions in sorted order. */
        array(): Uint32Array;
        /** LCP of each suffix with the previous one in `array()` order (0 first). */
        lcp(): Uint32Array;
        /** How many times `pattern` occurs (0 for the empty pattern). */
        count(pattern: string | Uint8Array): number;
        /** Ascending start positions of every occurrence of `pattern`. */
        find(pattern: string | Uint8Array): number[];
        /** The longest substring that occurs at least twice, or `undefined`. */
        longestRepeat(): { start: number; length: number } | undefined;
        /** The text length (UTF-16 units for a string, bytes otherwise). */
        readonly length: number;
        /** Writes the structure to `path` atomically; see the module notes on FILES. */
        save(path: import("dyna:file").Path, opts?: PersistSaveOptions): PersistSaveResult;
        /** Reads a file written by save(); see the module notes on FILES for options and errors. */
        static load(path: import("dyna:file").Path, opts?: PersistLoadOptions): SuffixArray;
    }

    /** Ordered map on numeric keys as a B-tree of order 32; iterable over [key, value]; its serialize format differs from SortedMap's. */
    class BTree<V = unknown> {
        /** Creates an empty tree. NaN keys throw `TypeError` ("keys must be ordered numbers, not NaN"). */
        constructor();
        /** Rebuilds a tree from its record. */
        static deserialize<T>(this: new () => BTree<T>, bytes: Uint8Array | ArrayBuffer): BTree<T>;
        /** Reads a file written by save(); see the module notes on FILES for options and errors. */
        static load<T>(this: new () => BTree<T>, path: import("dyna:file").Path, opts?: PersistLoadOptions): BTree<T>;
        /** Inserts or replaces. O(log n). */
        set(k: number, v: V): this;
        /** The value. O(log n). */
        get(k: number): V | undefined;
        /** Whether the key is present. O(log n). */
        has(k: number): boolean;
        /** Removes the key, reporting whether it was present. O(log n). */
        delete(k: number): boolean;
        /** The smallest key. */
        firstKey(): number | undefined;
        /** The largest key. */
        lastKey(): number | undefined;
        /** The largest key <= k (ceilKey: the smallest >= k). */
        floorKey(k: number): number | undefined;
        /** The smallest key ≥ `k`; `undefined` if none. */
        ceilKey(k: number): number | undefined;
        /** Ascending `[k, v]` pairs in `[lo, hi]`. */
        rangeQuery(lo: number, hi: number): [number, V][];
        /** Ascending keys. */
        keys(): number[];
        /** The key count. */
        readonly size: number;
        [Symbol.iterator](): Iterator<[number, V]>;
        /** The contents as a compact type-tagged record. */
        serialize(): Uint8Array;
        /** Writes the structure to `path` atomically; see the module notes on FILES. */
        save(path: import("dyna:file").Path, opts?: PersistSaveOptions): PersistSaveResult;
    }

    /** Double-ended queue with O(1) push/pop at both ends. */
    class Deque<V = unknown> {
        /** Creates an empty deque; the buffer grows from 8. */
        constructor();
        /** Rebuilds a deque from its record. */
        static deserialize<T>(this: new () => Deque<T>, bytes: Uint8Array | ArrayBuffer): Deque<T>;
        /** Reads a file written by save(); see the module notes on FILES for options and errors. */
        static load<T>(this: new () => Deque<T>, path: import("dyna:file").Path, opts?: PersistLoadOptions): Deque<T>;
        /** Appends at the back and returns the new length. */
        pushBack(v: V): number;
        /** Prepends at the front and returns the new length. */
        pushFront(v: V): number;
        /** Removes and returns the front; `undefined` when empty. */
        popFront(): V | undefined;
        /** Removes and returns the back; `undefined` when empty. */
        popBack(): V | undefined;
        /** The front without removal. */
        peekFront(): V | undefined;
        /** The back without removal. */
        peekBack(): V | undefined;
        /** The element at index `i`. */
        get(i: number): V | undefined;
        /** The element count. */
        readonly length: number;
        /** A snapshot of the elements. */
        toArray(): V[];
        [Symbol.iterator](): Iterator<V>;
        /** The contents as a compact type-tagged record. */
        serialize(): Uint8Array;
        /** Writes the structure to `path` atomically; see the module notes on FILES. */
        save(path: import("dyna:file").Path, opts?: PersistSaveOptions): PersistSaveResult;
    }

    /** Doubly-linked list of JS values; element identity is stable. */
    class List<V = unknown> {
        /** Creates an empty list. */
        constructor();
        /** Rebuilds a list from its record. */
        static deserialize<T>(this: new () => List<T>, bytes: Uint8Array | ArrayBuffer): List<T>;
        /** Reads a file written by save(); see the module notes on FILES for options and errors. */
        static load<T>(this: new () => List<T>, path: import("dyna:file").Path, opts?: PersistLoadOptions): List<T>;
        /** Prepends and returns the new length. */
        pushFront(v: V): number;
        /** Appends and returns the new length. */
        pushBack(v: V): number;
        /** Removes and returns the front. */
        popFront(): V | undefined;
        /** Removes and returns the back. */
        popBack(): V | undefined;
        /** The front without removal. */
        front(): V | undefined;
        /** The back without removal. */
        back(): V | undefined;
        /** The element count. */
        readonly length: number;
        /** A head-to-tail snapshot. */
        toArray(): V[];
        [Symbol.iterator](): Iterator<V>;
        /** The contents as a compact type-tagged record. */
        serialize(): Uint8Array;
        /** Writes the structure to `path` atomically; see the module notes on FILES. */
        save(path: import("dyna:file").Path, opts?: PersistSaveOptions): PersistSaveResult;
    }

    /** Fixed-capacity circular buffer; push overwrites the oldest when full. */
    class RingBuffer<V = unknown> {
        /** Creates a fixed-capacity circular buffer. */
        constructor(capacity: number);
        /** Rebuilds a buffer from its record. */
        static deserialize<T>(this: new (capacity: number) => RingBuffer<T>, bytes: Uint8Array | ArrayBuffer): RingBuffer<T>;
        /** Reads a file written by save(); see the module notes on FILES for options and errors. */
        static load<T>(this: new (capacity: number) => RingBuffer<T>, path: import("dyna:file").Path, opts?: PersistLoadOptions): RingBuffer<T>;
        /** Appends, evicting the oldest when full; returns the count. */
        push(v: V): number;
        /** The element at index `i`. */
        get(i: number): V | undefined;
        /** The current count. */
        readonly length: number;
        /** The capacity. */
        readonly capacity: number;
        /** Whether the buffer holds `capacity` elements. */
        readonly full: boolean;
        /** The elements in insertion order. */
        toArray(): V[];
        [Symbol.iterator](): Iterator<V>;
        /** The contents as a compact type-tagged record. */
        serialize(): Uint8Array;
        /** Writes the structure to `path` atomically; see the module notes on FILES. */
        save(path: import("dyna:file").Path, opts?: PersistSaveOptions): PersistSaveResult;
    }

    /** Dynamic bit set backed by 64-bit words. */
    class BitSet {
        /** `nbits` is an initial capacity hint (up to 2^30 bits); the set grows on demand. `cache` (default off) keeps
         * derived acceleration state: `count` (or `cache: true`) keeps the population count up to date on set/clear/flip
         * and recounts once after and/or/xor, so `count` is O(1) instead of O(words); it costs no memory and nothing
         * measurable per write. The cache never changes results or serialize() bytes, and a deserialized or loaded set
         * starts with it off. Unknown option keys throw TypeError. */
        constructor(nbits?: number, options?: { cache?: boolean | { count?: boolean } });
        /** Rebuilds a bit set from its record. */
        static deserialize(bytes: Uint8Array | ArrayBuffer): BitSet;
        /** Reads a file written by save(); see the module notes on FILES for options and errors. */
        static load(path: import("dyna:file").Path, opts?: PersistLoadOptions): BitSet;
        /** Sets the bit; indices above the current size grow the set. Refuses indices ≥ 2^30 with `RangeError`. */
        set(i: number): this;
        /** Clears the bit. */
        clear(i: number): this;
        /** Flips the bit; indices above the current size grow the set. Refuses indices ≥ 2^30 with `RangeError`. */
        flip(i: number): this;
        /** False when the bit is past the allocated size. */
        get(i: number): boolean;
        /** The first set bit at position >= from, or -1. */
        nextSet(from: number): number;
        /** Number of set bits. */
        readonly count: number;
        /** In-place word AND; bits past `other` clear. */
        and(other: BitSet): this;
        /** In-place word OR; `this` grows as needed. */
        or(other: BitSet): this;
        /** In-place word XOR; `this` grows as needed. */
        xor(other: BitSet): this;
        /** Ascending indices of set bits. */
        toArray(): number[];
        [Symbol.iterator](): Iterator<number>;
        /** The contents as a compact type-tagged record. */
        serialize(): Uint8Array;
        /** Writes the structure to `path` atomically; see the module notes on FILES. */
        save(path: import("dyna:file").Path, opts?: PersistSaveOptions): PersistSaveResult;
    }

    /** Disjoint-set forest with path halving and union by rank. */
    class UnionFind {
        /** Elements are 0..n-1 (n up to 2^26). */
        constructor(n?: number);
        /** Rebuilds a forest from its record. */
        static deserialize(bytes: Uint8Array | ArrayBuffer): UnionFind;
        /** Reads a file written by save(); see the module notes on FILES for options and errors. */
        static load(path: import("dyna:file").Path, opts?: PersistLoadOptions): UnionFind;
        /** The representative of the set containing x. */
        find(x: number): number;
        /** Merges the sets of x and y; false when they were already joined. */
        union(x: number, y: number): boolean;
        /** Whether both elements are in the same component. */
        connected(x: number, y: number): boolean;
        /** Number of disjoint components. */
        readonly count: number;
        /** Element count. */
        readonly size: number;
        /** The contents as a compact type-tagged record. */
        serialize(): Uint8Array;
        /** Writes the structure to `path` atomically; see the module notes on FILES. */
        save(path: import("dyna:file").Path, opts?: PersistSaveOptions): PersistSaveResult;
    }

    /** Fenwick tree over a fixed-size vector of doubles. */
    class Fenwick {
        /** Creates a Fenwick tree. */
        constructor(n: number);
        /** Rebuilds a tree from its record. */
        static deserialize(bytes: Uint8Array | ArrayBuffer): Fenwick;
        /** Reads a file written by save(); see the module notes on FILES for options and errors. */
        static load(path: import("dyna:file").Path, opts?: PersistLoadOptions): Fenwick;
        /** Adds `delta` to position `i`. Out-of-range throws `RangeError`. */
        update(i: number, delta: number): this;
        /** Sum of positions [0..i] inclusive. */
        prefixSum(i: number): number;
        /** Sum of [lo..hi] inclusive; 0 for an empty range. */
        rangeQuery(lo: number, hi: number): number;
        /** The number of positions. */
        readonly size: number;
        /** The contents as a compact type-tagged record. */
        serialize(): Uint8Array;
        /** Writes the structure to `path` atomically; see the module notes on FILES. */
        save(path: import("dyna:file").Path, opts?: PersistSaveOptions): PersistSaveResult;
    }

    /** Iterative segment tree with an associative fold: "sum"|"min"|"max". */
    class SegTree {
        /** Creates a segment tree. Defaults: op "sum". */
        constructor(n: number, op?: SegOp);
        /** Rebuilds a tree from its record. */
        static deserialize(bytes: Uint8Array | ArrayBuffer): SegTree;
        /** Reads a file written by save(); see the module notes on FILES for options and errors. */
        static load(path: import("dyna:file").Path, opts?: PersistLoadOptions): SegTree;
        /** Assigns leaf `i`, re-folding the path to the root. */
        update(i: number, value: number): this;
        /** The fold over `[lo..hi]`; the identity for an empty (`lo > hi`) range. */
        rangeQuery(lo: number, hi: number): number;
        /** The number of leaves. */
        readonly size: number;
        /** The contents as a compact type-tagged record. */
        serialize(): Uint8Array;
        /** Writes the structure to `path` atomically; see the module notes on FILES. */
        save(path: import("dyna:file").Path, opts?: PersistSaveOptions): PersistSaveResult;
    }

    /** Probabilistic set membership; no false negatives. */
    class BloomFilter {
        /** `bits` up to 2^30; `hashes` defaults to 3 (clamped to 1..32). */
        constructor(bits: number, hashes?: number);
        /** Rebuilds a filter from its record; accepts every size the constructor does. A record claiming more than 2^30 bits, or carrying fewer words than it claims, is a TypeError. The sparse encoding stops at 2^22 words (a tiny record must not decode to hundreds of MB); serialize writes larger filters raw, 8 bytes per word. */
        static deserialize(bytes: Uint8Array | ArrayBuffer): BloomFilter;
        /** Reads a file written by save(); see the module notes on FILES for options and errors. */
        static load(path: import("dyna:file").Path, opts?: PersistLoadOptions): BloomFilter;
        /** Sets the key's `hashes` bits. */
        add(key: string): this;
        /** False means definitely absent; true means probably present. */
        mayContain(key: string): boolean;
        /** The number of bits. */
        readonly bits: number;
        /** The number of hashes. */
        readonly hashes: number;
        /** The contents as a compact type-tagged record. */
        serialize(): Uint8Array;
        /** Writes the structure to `path` atomically; see the module notes on FILES. */
        save(path: import("dyna:file").Path, opts?: PersistSaveOptions): PersistSaveResult;
    }

    /** Set of byte strings with prefix queries. */
    class Trie {
        /** Creates an empty trie. */
        constructor();
        /** Rebuilds a trie from its record. */
        static deserialize(bytes: Uint8Array | ArrayBuffer): Trie;
        /** Reads a file written by save(); see the module notes on FILES for options and errors. */
        static load(path: import("dyna:file").Path, opts?: PersistLoadOptions): Trie;
        /** Stores the key; re-inserting an existing key is a no-op. */
        insert(key: string): this;
        /** Whether the key is present. */
        has(key: string): boolean;
        /** Removes the key, reporting whether it was present; the branch the key leaves empty is unlinked (its node memory is kept until the trie is freed), so every reachable node still leads to a stored key. */
        delete(key: string): boolean;
        /** Every stored key starting with `prefix`. Ascending order is *not* guaranteed (sibling insertion order). */
        keysWithPrefix(prefix: string): string[];
        /** Whether some stored key starts with `prefix` (UTF-8 bytes); "" is true for a non-empty trie. One walk of O(len prefix), allocating nothing and building no key list, unlike keysWithPrefix(prefix).length > 0. */
        hasPrefix(prefix: string): boolean;
        /** The longest stored key that is a prefix of str, or "". */
        longestPrefix(str: string): string;
        /** The number of stored keys. */
        readonly size: number;
        [Symbol.iterator](): Iterator<string>;
        /** The contents as a compact type-tagged record. */
        serialize(): Uint8Array;
        /** Writes the structure to `path` atomically; see the module notes on FILES. */
        save(path: import("dyna:file").Path, opts?: PersistSaveOptions): PersistSaveResult;
    }

    /** String key to uint64 count, saturating at 2^64-1; counts above 2^53-1 read back as the nearest double. */
    class Multiset {
        /** Creates an empty multiset. */
        constructor();
        /** Rebuilds a multiset from its record. */
        static deserialize(bytes: Uint8Array | ArrayBuffer): Multiset;
        /** Reads a file written by save(); see the module notes on FILES for options and errors. */
        static load(path: import("dyna:file").Path, opts?: PersistLoadOptions): Multiset;
        /** The key's new count. Defaults: n 1. */
        add(key: string, n?: number): number;
        /** Subtracts, returning the new count (flooring at 0; the record is dropped at 0). Defaults: n 1. */
        remove(key: string, n?: number): number;
        /** The key's count. */
        count(key: string): number;
        /** Whether the key is present. */
        has(key: string): boolean;
        /** Sets the count exactly. */
        setCount(key: string, count: number): this;
        /** Removes the key entirely, reporting whether it was present. */
        delete(key: string): boolean;
        /** Drops all entries. */
        clear(): void;
        /** The distinct keys. */
        elementSet(): string[];
        /** The keys with their counts. */
        entrySet(): [string, number][];
        /** Distinct key count. */
        readonly size: number;
        /** Sum of all counts. */
        readonly totalSize: number;
        [Symbol.iterator](): Iterator<[string, number]>;
        /** The contents as a compact type-tagged record. */
        serialize(): Uint8Array;
        /** Writes the structure to `path` atomically; see the module notes on FILES. */
        save(path: import("dyna:file").Path, opts?: PersistSaveOptions): PersistSaveResult;
    }

    /** String key to a growing value array. */
    class Multimap<V = unknown> {
        /** Creates an empty multimap. */
        constructor();
        /** Rebuilds a multimap from its record. */
        static deserialize<T>(this: new () => Multimap<T>, bytes: Uint8Array | ArrayBuffer): Multimap<T>;
        /** Reads a file written by save(); see the module notes on FILES for options and errors. */
        static load<T>(this: new () => Multimap<T>, path: import("dyna:file").Path, opts?: PersistLoadOptions): Multimap<T>;
        /** Appends one value. */
        put(key: string, value: V): this;
        /** A fresh array of all values for the key (empty if none). */
        get(key: string): V[];
        /** The number of values for the key. */
        count(key: string): number;
        /** Removes every value for the key; returns how many. */
        delete(key: string): number;
        /** Removes the value at `index`. Both arguments required (`TypeError` otherwise). */
        removeAt(key: string, index: number): V | undefined;
        /** The distinct keys. */
        keys(): string[];
        /** Flattened pairs. */
        entries(): [string, V][];
        /** Total values. */
        readonly size: number;
        /** Distinct keys. */
        readonly keyCount: number;
        [Symbol.iterator](): Iterator<[string, V]>;
        /** The contents as a compact type-tagged record. */
        serialize(): Uint8Array;
        /** Writes the structure to `path` atomically; see the module notes on FILES. */
        save(path: import("dyna:file").Path, opts?: PersistSaveOptions): PersistSaveResult;
    }

    /** Two-way string-to-string map. */
    class BiMap {
        /** Creates an empty bimap. */
        constructor();
        /** Rebuilds a bimap from its record. */
        static deserialize(bytes: Uint8Array | ArrayBuffer): BiMap;
        /** Reads a file written by save(); see the module notes on FILES for options and errors. */
        static load(path: import("dyna:file").Path, opts?: PersistLoadOptions): BiMap;
        /** Throws `TypeError` if the value is already bound to another key. */
        set(key: string, value: string): this;
        /** Sets the pair, evicting any entry that already holds the value. */
        forceSet(key: string, value: string): this;
        /** Forward lookup. */
        get(key: string): string | undefined;
        /** The key mapped to `value` (the inverse lookup). */
        keyOf(value: string): string | undefined;
        /** Whether the key is bound. */
        has(key: string): boolean;
        /** Whether the value is bound. */
        hasValue(value: string): boolean;
        /** Drops the key and its value, reporting whether it was present. */
        delete(key: string): boolean;
        /** Drops the value and its key, reporting whether it was present. */
        deleteValue(value: string): boolean;
        /** The forward pairs. */
        entries(): [string, string][];
        /** The inverse pairs. */
        inverseEntries(): [string, string][];
        /** Drops all pairs. */
        clear(): void;
        /** The number of pairs. */
        readonly size: number;
        [Symbol.iterator](): Iterator<[string, string]>;
        /** The contents as a compact type-tagged record. */
        serialize(): Uint8Array;
        /** Writes the structure to `path` atomically; see the module notes on FILES. */
        save(path: import("dyna:file").Path, opts?: PersistSaveOptions): PersistSaveResult;
    }

    /** Sparse two-dimensional string-to-string-to-value map. */
    class Table<V = unknown> {
        /** Creates an empty table. */
        constructor();
        /** Rebuilds a table from its record. */
        static deserialize<T>(this: new () => Table<T>, bytes: Uint8Array | ArrayBuffer): Table<T>;
        /** Reads a file written by save(); see the module notes on FILES for options and errors. */
        static load<T>(this: new () => Table<T>, path: import("dyna:file").Path, opts?: PersistLoadOptions): Table<T>;
        /** Sets a cell, replacing the old value. */
        put(row: string, col: string, value: V): this;
        /** The cell value. */
        get(row: string, col: string): V | undefined;
        /** Whether the cell is present. */
        has(row: string, col: string): boolean;
        /** Removes a cell, reporting whether it was present. */
        delete(row: string, col: string): boolean;
        row(r: string): [string, V][];
        /** The column's cells; a linear scan of the sparse cell array. */
        column(c: string): [string, V][];
        /** Every occupied cell. */
        cells(): [string, string, V][];
        /** The cell count. */
        readonly size: number;
        [Symbol.iterator](): Iterator<[string, string, V]>;
        /** The contents as a compact type-tagged record. */
        serialize(): Uint8Array;
        /** Writes the structure to `path` atomically; see the module notes on FILES. */
        save(path: import("dyna:file").Path, opts?: PersistSaveOptions): PersistSaveResult;
    }

    /** A set of numeric intervals [lo, hi), kept disjoint and merged on insert. */
    class RangeSet {
        /** Creates an empty range set. */
        constructor();
        /** Rebuilds a range set from its record. */
        static deserialize(bytes: Uint8Array | ArrayBuffer): RangeSet;
        /** Reads a file written by save(); see the module notes on FILES for options and errors. */
        static load(path: import("dyna:file").Path, opts?: PersistLoadOptions): RangeSet;
        /** Unions an interval; overlapping neighbours merge as needed. NaN bounds throw `RangeError`. */
        add(lo: number, hi: number): this;
        /** Subtracts an interval; overlapping neighbours split as needed. */
        remove(lo: number, hi: number): this;
        /** Whether the point is covered. */
        contains(x: number): boolean;
        /** True when [lo, hi) lies entirely inside the set. */
        encloses(lo: number, hi: number): boolean;
        /** Whether the interval overlaps any stored range. */
        intersects(lo: number, hi: number): boolean;
        /** The disjoint ranges in order. */
        ranges(): [number, number][];
        /** The gaps of [lo, hi) outside the set. */
        complement(lo: number, hi: number): [number, number][];
        /** Drops all ranges. */
        clear(): void;
        /** The number of disjoint ranges. */
        readonly size: number;
        /** The total covered length. */
        readonly measure: number;
        [Symbol.iterator](): Iterator<[number, number]>;
        /** The contents as a compact type-tagged record. */
        serialize(): Uint8Array;
        /** Writes the structure to `path` atomically; see the module notes on FILES. */
        save(path: import("dyna:file").Path, opts?: PersistSaveOptions): PersistSaveResult;
    }

    /** Map from numeric intervals [lo, hi) to values; overlapping puts split. */
    class RangeMap<V = unknown> {
        /** Creates an empty range map. */
        constructor();
        /** Rebuilds a range map from its record. */
        static deserialize<T>(this: new () => RangeMap<T>, bytes: Uint8Array | ArrayBuffer): RangeMap<T>;
        /** Reads a file written by save(); see the module notes on FILES for options and errors. */
        static load<T>(this: new () => RangeMap<T>, path: import("dyna:file").Path, opts?: PersistLoadOptions): RangeMap<T>;
        /** Assigns the whole span; an empty range (`hi <= lo`) stores nothing. */
        put(lo: number, hi: number, value: V): this;
        /** The value of the range containing `x`, or `undefined` if uncovered. */
        get(x: number): V | undefined;
        /** Clears a span, splitting neighbour ranges. */
        remove(lo: number, hi: number): this;
        /** The stored ranges in order. */
        entries(): [number, number, V][];
        /** The number of stored ranges. */
        readonly size: number;
        [Symbol.iterator](): Iterator<[number, number, V]>;
        /** The contents as a compact type-tagged record. */
        serialize(): Uint8Array;
        /** Writes the structure to `path` atomically; see the module notes on FILES. */
        save(path: import("dyna:file").Path, opts?: PersistSaveOptions): PersistSaveResult;
    }

    /** Interval store with overlap enumeration ([lo, hi) spans). */
    class IntervalTree<V = unknown> {
        /** Creates an empty interval tree. */
        constructor();
        /** Rebuilds an interval tree from its record. */
        static deserialize<T>(this: new () => IntervalTree<T>, bytes: Uint8Array | ArrayBuffer): IntervalTree<T>;
        /** Reads a file written by save(); see the module notes on FILES for options and errors. */
        static load<T>(this: new () => IntervalTree<T>, path: import("dyna:file").Path, opts?: PersistLoadOptions): IntervalTree<T>;
        /** `lo` must not exceed `hi`; an inverted pair or a NaN bound is a RangeError. */
        insert(lo: number, hi: number, value: V): this;
        /** Entries whose interval overlaps [lo, hi); at(x) returns those containing x. */
        overlapping(lo: number, hi: number): [number, number, V][];
        /** The degenerate point query `[x, x]`. */
        at(x: number): [number, number, V][];
        /** The interval count. */
        readonly size: number;
        /** The contents as a compact type-tagged record. */
        serialize(): Uint8Array;
        /** Writes the structure to `path` atomically; see the module notes on FILES. */
        save(path: import("dyna:file").Path, opts?: PersistSaveOptions): PersistSaveResult;
    }

    /** Count-min sketch: depth rows of width saturating counters. */
    class CountMinSketch {
        /** `depth` defaults to 5 (at most 64); width * depth must not exceed 2^24 counters. */
        constructor(width: number, depth?: number);
        /** Rebuilds a sketch from its record; accepts every size the constructor does. A record claiming more than 2^24 counters, or carrying fewer counters than it claims, is a TypeError. The sparse encoding stops at 2^22 counters (a tiny record must not decode to hundreds of MB); serialize writes larger sketches raw, 8 bytes per counter. */
        static deserialize(bytes: Uint8Array | ArrayBuffer): CountMinSketch;
        /** Reads a file written by save(); see the module notes on FILES for options and errors. */
        static load(path: import("dyna:file").Path, opts?: PersistLoadOptions): CountMinSketch;
        /** Counts `n` occurrences. Defaults: n 1. */
        add(key: string, n?: number): this;
        /** The estimate for the key; an overestimate (an upper bound). */
        count(key: string): number;
        /** Adds another sketch of the same shape into this one. */
        merge(other: CountMinSketch): this;
        /** The number of counters per row. */
        readonly width: number;
        /** The number of rows. */
        readonly depth: number;
        /** The total number of occurrences added. */
        readonly totalCount: number;
        /** The contents as a compact type-tagged record. */
        serialize(): Uint8Array;
        /** Writes the structure to `path` atomically; see the module notes on FILES. */
        save(path: import("dyna:file").Path, opts?: PersistSaveOptions): PersistSaveResult;
    }

    /** Cardinality estimator over string keys. */
    class HyperLogLog {
        /** `precision` is 4..18, default 14 (16 KiB of registers). */
        constructor(precision?: number);
        /** Rebuilds an estimator from its record. */
        static deserialize(bytes: Uint8Array | ArrayBuffer): HyperLogLog;
        /** Reads a file written by save(); see the module notes on FILES for options and errors. */
        static load(path: import("dyna:file").Path, opts?: PersistLoadOptions): HyperLogLog;
        /** Folds the key's hash into one register. */
        add(key: string): this;
        /** The cardinality estimate. */
        count(): number;
        /** Unions the registers. Requires equal precision (else `TypeError`). */
        merge(other: HyperLogLog): this;
        /** The exponent: the estimator holds `2^precision` registers. */
        readonly precision: number;
        /** The number of registers (`2^precision`). */
        readonly registers: number;
        /** The contents as a compact type-tagged record. */
        serialize(): Uint8Array;
        /** Writes the structure to `path` atomically; see the module notes on FILES. */
        save(path: import("dyna:file").Path, opts?: PersistSaveOptions): PersistSaveResult;
    }

    /** Redis-style sorted set: unique string members ordered by (score, member bytes), with O(log n) rank queries. */
    class ZSet {
        /** Creates an empty sorted set. */
        constructor();
        /** Rebuilds a sorted set from its record. */
        static deserialize(bytes: Uint8Array | ArrayBuffer): ZSet;
        /** Reads a file written by save(); see the module notes on FILES for options and errors. */
        static load(path: import("dyna:file").Path, opts?: PersistLoadOptions): ZSet;
        /** Sets the member's score; true when the member is new. A NaN score is a TypeError. */
        add(member: string, score: number): boolean;
        /** Adds `delta` to the score (a missing member starts at 0) and returns the new score. */
        incrBy(member: string, delta: number): number;
        /** The member's score, or undefined. */
        score(member: string): number | undefined;
        /** Whether the member is present. */
        has(member: string): boolean;
        /** Removes the member; false when it was absent. */
        delete(member: string): boolean;
        /** The 0-based position in ascending order, or undefined. */
        rank(member: string): number | undefined;
        /** The 0-based position in descending order, or undefined. */
        revRank(member: string): number | undefined;
        /** The entry at a 0-based rank; a negative index counts from the end. */
        at(index: number): [string, number] | undefined;
        /** Entries from rank `start` to `stop` inclusive (negatives count from the end, as ZRANGE). */
        range(start: number, stop: number, opts?: { reverse?: boolean }): [string, number][];
        /** Every entry in ascending order. */
        entries(): [string, number][];
        /** Entries with min <= score <= max; `reverse` walks from max down. */
        rangeByScore(min: number, max: number, opts?: { offset?: number; limit?: number; reverse?: boolean }): [string, number][];
        /** The number of members with min <= score <= max, without materialising them. */
        countByScore(min: number, max: number): number;
        /** Removes members with min <= score <= max and returns how many. */
        removeRangeByScore(min: number, max: number): number;
        /** Removes ranks `start`..`stop` inclusive (ZREMRANGEBYRANK) and returns how many. */
        removeRangeByRank(start: number, stop: number): number;
        /** Removes and returns the lowest entry. */
        popMin(): [string, number] | undefined;
        /** Removes and returns the highest entry. */
        popMax(): [string, number] | undefined;
        /** Removes every member. */
        clear(): void;
        /** The member count. */
        readonly size: number;
        [Symbol.iterator](): Iterator<[string, number]>;
        /** The contents as a compact type-tagged record. */
        serialize(): Uint8Array;
        /** Writes the structure to `path` atomically; see the module notes on FILES. */
        save(path: import("dyna:file").Path, opts?: PersistSaveOptions): PersistSaveResult;
    }

    /** Sorted map over string keys in UTF-8 byte order (code-point order), with O(1) get and O(log n) seeks and ranks. */
    class OrderedMap<V = unknown> {
        /** Creates an empty map. */
        constructor();
        /** Rebuilds a map from its record. */
        static deserialize<T>(this: new () => OrderedMap<T>, bytes: Uint8Array | ArrayBuffer): OrderedMap<T>;
        /** Reads a file written by save(); see the module notes on FILES for options and errors. */
        static load<T>(this: new () => OrderedMap<T>, path: import("dyna:file").Path, opts?: PersistLoadOptions): OrderedMap<T>;
        /** Inserts or replaces the value for `key`. */
        set(key: string, value: V): this;
        /** The value for `key`, or undefined. */
        get(key: string): V | undefined;
        /** Whether `key` is present. */
        has(key: string): boolean;
        /** Removes `key`; false when it was absent. */
        delete(key: string): boolean;
        /** The smallest key. */
        firstKey(): string | undefined;
        /** The largest key. */
        lastKey(): string | undefined;
        /** The largest key <= `key`. */
        floorKey(key: string): string | undefined;
        /** The smallest key >= `key`. */
        ceilKey(key: string): string | undefined;
        /** The largest key < `key`. */
        lowerKey(key: string): string | undefined;
        /** The smallest key > `key`. */
        higherKey(key: string): string | undefined;
        /** The number of keys < `key`. */
        rank(key: string): number;
        /** The entry at a 0-based position; a negative index counts from the end. */
        at(index: number): [string, V] | undefined;
        /** Entries with lo <= key <= hi; `reverse` walks from hi down. */
        rangeQuery(lo: string, hi: string, opts?: { limit?: number; reverse?: boolean }): [string, V][];
        /** Entries whose key starts with `prefix`, in key order. */
        prefix(prefix: string, opts?: { limit?: number }): [string, V][];
        /** Removes keys with lo <= key <= hi and returns how many. */
        deleteRange(lo: string, hi: string): number;
        /** Every key in order. */
        keys(): string[];
        /** Every entry in key order. */
        entries(): [string, V][];
        /** Removes every key. */
        clear(): void;
        /** The key count. */
        readonly size: number;
        [Symbol.iterator](): Iterator<[string, V]>;
        /** The contents as a compact type-tagged record. */
        serialize(): Uint8Array;
        /** Writes the structure to `path` atomically; see the module notes on FILES. */
        save(path: import("dyna:file").Path, opts?: PersistSaveOptions): PersistSaveResult;
    }

    /** Compressed set of uint32 values (array, bitmap and full 2^16 containers); set algebra runs in C per 64 Ki chunk. */
    class RoaringBitmap {
        /** Creates a bitmap, optionally from integers in 0..2^32-1. */
        constructor(values?: ArrayLike<number> | Uint32Array | Int32Array);
        /** Rebuilds a bitmap from its record. */
        static deserialize(bytes: Uint8Array | ArrayBuffer): RoaringBitmap;
        /** Reads a file written by save(); see the module notes on FILES for options and errors. */
        static load(path: import("dyna:file").Path, opts?: PersistLoadOptions): RoaringBitmap;
        /** Adds `x` (an integer in 0..2^32-1). */
        add(x: number): this;
        /** Adds `x`; true when it was not already present. */
        tryAdd(x: number): boolean;
        /** Adds every value; the input is sorted first, so any order is fine. */
        addMany(values: ArrayLike<number> | Uint32Array | Int32Array): this;
        /** Adds the half-open range [lo, hi); bounds are integers in 0..2^32. */
        addRange(lo: number, hi: number): this;
        /** Removes the half-open range [lo, hi). */
        removeRange(lo: number, hi: number): this;
        /** Removes `x`; false when it was absent. */
        delete(x: number): boolean;
        /** Whether `x` is present. */
        has(x: number): boolean;
        /** The number of values <= `x`. */
        rank(x: number): number;
        /** The value at a 0-based position in ascending order. */
        select(i: number): number | undefined;
        /** The number of values in [lo, hi). */
        rangeCardinality(lo: number, hi: number): number;
        /** Intersects in place. */
        and(other: RoaringBitmap): this;
        /** Unions in place. */
        or(other: RoaringBitmap): this;
        /** Symmetric difference in place. */
        xor(other: RoaringBitmap): this;
        /** Removes every value of `other` in place. */
        andNot(other: RoaringBitmap): this;
        /** |this AND other| without building the result. */
        andCardinality(other: RoaringBitmap): number;
        /** |this OR other| without building the result. */
        orCardinality(other: RoaringBitmap): number;
        /** |this XOR other| without building the result. */
        xorCardinality(other: RoaringBitmap): number;
        /** |this AND NOT other| without building the result. */
        andNotCardinality(other: RoaringBitmap): number;
        /** Whether the two share a value. */
        intersects(other: RoaringBitmap): boolean;
        /** Whether every value is also in `other`. */
        isSubsetOf(other: RoaringBitmap): boolean;
        /** Whether both hold the same values. */
        equals(other: RoaringBitmap): boolean;
        /** |A AND B| / |A OR B|; 1 for two empty bitmaps. */
        jaccard(other: RoaringBitmap): number;
        /** The smallest value. */
        min(): number | undefined;
        /** The largest value. */
        max(): number | undefined;
        /** The values in ascending order; more than 2^28 values is a RangeError. */
        toArray(): Uint32Array;
        /** An independent copy. */
        clone(): RoaringBitmap;
        /** Removes every value. */
        clear(): void;
        /** Whether no value is present. */
        isEmpty(): boolean;
        /** The number of values (up to 2^32). */
        readonly size: number;
        /** Approximate bytes held by the containers. */
        readonly bytes: number;
        [Symbol.iterator](): Iterator<number>;
        /** The contents as a compact type-tagged record. */
        serialize(): Uint8Array;
        /** Writes the structure to `path` atomically; see the module notes on FILES. */
        save(path: import("dyna:file").Path, opts?: PersistSaveOptions): PersistSaveResult;
    }

    /** Merging t-digest (k1 scale): streaming quantiles whose error shrinks toward the tails. */
    class TDigest {
        /** `compression` (delta, 10..10000) defaults to 100; about 1.6 * delta centroids are kept. */
        constructor(compression?: number);
        /** Rebuilds a digest from its record. */
        static deserialize(bytes: Uint8Array | ArrayBuffer): TDigest;
        /** Reads a file written by save(); see the module notes on FILES for options and errors. */
        static load(path: import("dyna:file").Path, opts?: PersistLoadOptions): TDigest;
        /** Adds a finite value with a positive weight (default 1). */
        add(x: number, weight?: number): this;
        /** Adds every value of an array or a typed array; a non-finite value is a RangeError. */
        addMany(values: ArrayLike<number> | Float64Array | Float32Array): this;
        /** The estimated value at quantile q in 0..1; NaN when empty. Reads (quantile, quantiles, cdf, centroids, merge's source, serialize) never flush the insert buffer, so a digest that was queried or checkpointed estimates exactly like one that was not. */
        quantile(q: number): number;
        /** quantile() for each q, in one call. */
        quantiles(qs: ArrayLike<number> | Float64Array): Float64Array;
        /** The estimated fraction of weight <= x; NaN when empty. */
        cdf(x: number): number;
        /** Adds another digest's centroids (and its unflushed values) into this one; `other` is left unchanged. */
        merge(other: TDigest): this;
        /** The total weight added. */
        readonly count: number;
        /** The smallest value added, or undefined. */
        readonly min: number | undefined;
        /** The largest value added, or undefined. */
        readonly max: number | undefined;
        /** The exact weighted mean, or undefined. */
        readonly mean: number | undefined;
        /** The centroid count a compression of the current contents yields. */
        readonly centroids: number;
        /** The delta given to the constructor. */
        readonly compression: number;
        /** The contents as a compact type-tagged record, insert buffer included: deserialize() restores the exact state, so later estimates match a digest that was never serialized. Records from earlier versions (no buffer) still load. */
        serialize(): Uint8Array;
        /** Writes the structure to `path` atomically; see the module notes on FILES. */
        save(path: import("dyna:file").Path, opts?: PersistSaveOptions): PersistSaveResult;
    }

    /** Space-Saving heavy hitters: k counters; any key seen more than total/k times is guaranteed to be listed. */
    class TopK {
        /** `k` is 1..2^20. */
        constructor(k: number);
        /** Rebuilds a summary from its record. */
        static deserialize(bytes: Uint8Array | ArrayBuffer): TopK;
        /** Reads a file written by save(); see the module notes on FILES for options and errors. */
        static load(path: import("dyna:file").Path, opts?: PersistLoadOptions): TopK;
        /** Counts `n` (default 1) occurrences; returns the key it evicted, if any. */
        add(key: string, n?: number): string | undefined;
        /** The estimated count (an upper bound), or 0 when untracked. */
        count(key: string): number;
        /** Whether the key holds a counter. */
        has(key: string): boolean;
        /** The overestimate bound for a tracked key; count - error is a lower bound. */
        error(key: string): number | undefined;
        /** [key, count, error] for every counter, highest count first (ties by key). */
        list(): [string, number, number][];
        /** The counter capacity. */
        readonly k: number;
        /** The counters in use. */
        readonly size: number;
        /** The total of every count added. */
        readonly total: number;
        /** The contents as a compact type-tagged record. */
        serialize(): Uint8Array;
        /** Writes the structure to `path` atomically; see the module notes on FILES. */
        save(path: import("dyna:file").Path, opts?: PersistSaveOptions): PersistSaveResult;
    }

    /** Cuckoo filter (4-slot buckets, 16-bit fingerprints): membership with deletion; false positives about 0.01%. */
    class CuckooFilter {
        /** Sized for `capacity` keys (1..2^26) at a 95% load factor. */
        constructor(capacity: number);
        /** Rebuilds a filter from its record (hashing is fixed, so records move between processes). */
        static deserialize(bytes: Uint8Array | ArrayBuffer): CuckooFilter;
        /** Reads a file written by save(); see the module notes on FILES for options and errors. */
        static load(path: import("dyna:file").Path, opts?: PersistLoadOptions): CuckooFilter;
        /** Inserts the key; false when the filter is full. */
        add(key: string): boolean;
        /** False means definitely absent; true means probably present. */
        mayContain(key: string): boolean;
        /** Removes one copy of a key that was added; deleting a never-added key can remove a colliding one. */
        delete(key: string): boolean;
        /** The number of keys held. */
        readonly size: number;
        /** The slot count. */
        readonly capacity: number;
        /** size / capacity. */
        readonly loadFactor: number;
        /** The contents as a compact type-tagged record. */
        serialize(): Uint8Array;
        /** Writes the structure to `path` atomically; see the module notes on FILES. */
        save(path: import("dyna:file").Path, opts?: PersistSaveOptions): PersistSaveResult;
    }

    /** Binary heap keyed by uint32 id: change any id's priority in O(log n) (Dijkstra, schedulers, timers). */
    class IndexedHeap {
        /** A min-heap by default; `max: true` pops the highest priority first. Ties pop the smaller id. */
        constructor(opts?: { max?: boolean });
        /** Rebuilds a heap from its record. */
        static deserialize(bytes: Uint8Array | ArrayBuffer): IndexedHeap;
        /** Reads a file written by save(); see the module notes on FILES for options and errors. */
        static load(path: import("dyna:file").Path, opts?: PersistLoadOptions): IndexedHeap;
        /** Inserts `id` or moves it to the new priority. */
        push(id: number, priority: number): this;
        /** push() for every pair; the two lists must be the same length. */
        pushMany(ids: ArrayLike<number> | Uint32Array | Int32Array, priorities: ArrayLike<number> | Float64Array): this;
        /** The id's priority, or undefined. */
        priority(id: number): number | undefined;
        /** Whether the id is queued. */
        has(id: number): boolean;
        /** Removes the id; false when it was absent. */
        delete(id: number): boolean;
        /** Removes and returns the top id. */
        pop(): number | undefined;
        /** Removes and returns the top [id, priority]. */
        popEntry(): [number, number] | undefined;
        /** Pops every id whose priority is <= limit (>= for a max-heap), at most `max` of them. */
        popUntil(limit: number, max?: number): Uint32Array;
        /** The top id without removing it. */
        peek(): number | undefined;
        /** The top priority. */
        peekPriority(): number | undefined;
        /** Removes every id. */
        clear(): void;
        /** The queued id count. */
        readonly size: number;
        /** Whether this is a max-heap. */
        readonly isMax: boolean;
        /** The contents as a compact type-tagged record. */
        serialize(): Uint8Array;
        /** Writes the structure to `path` atomically; see the module notes on FILES. */
        save(path: import("dyna:file").Path, opts?: PersistSaveOptions): PersistSaveResult;
    }

    /** Static k-d tree over finite points (1..64 dims, Euclidean); queries return point indices in input order. */
    class KDTree {
        /** From number[][], or from a flat array / typed array of n*dims coordinates with `dims`. */
        constructor(points: number[][] | ArrayLike<number> | Float64Array | Float32Array, dims?: number);
        /** Rebuilds a tree from its record. */
        static deserialize(bytes: Uint8Array | ArrayBuffer): KDTree;
        /** Reads a file written by save(); see the module notes on FILES for options and errors. */
        static load(path: import("dyna:file").Path, opts?: PersistLoadOptions): KDTree;
        /** The k nearest points, nearest first (ties by index). */
        knn(query: ArrayLike<number> | Float64Array, k: number): { indices: Int32Array; distances: Float64Array };
        /** The index of the nearest point, or -1 when empty. */
        nearest(query: ArrayLike<number> | Float64Array): number;
        /** knn() for m flat queries in one call; row i of the result is entries i*k..i*k+k-1 (k capped at size). */
        knnMany(queries: ArrayLike<number> | Float64Array, k: number): { indices: Int32Array; distances: Float64Array };
        /** Points within `r` of the query, nearest first. */
        radius(query: ArrayLike<number> | Float64Array, r: number): { indices: Int32Array; distances: Float64Array };
        /** Indices of points inside the closed box [lo, hi] per dimension, ascending. */
        box(lo: ArrayLike<number> | Float64Array, hi: ArrayLike<number> | Float64Array): Int32Array;
        /** The coordinates of point i. */
        point(i: number): Float64Array | undefined;
        /** The point count. */
        readonly size: number;
        /** The dimension count. */
        readonly dims: number;
        /** The contents as a compact type-tagged record. */
        serialize(): Uint8Array;
        /** Writes the structure to `path` atomically; see the module notes on FILES. */
        save(path: import("dyna:file").Path, opts?: PersistSaveOptions): PersistSaveResult;
    }

    /** Options of `new BKDTree` / `BKDTree.build`. */
    interface BKDTreeOptions {
        /** Value type (default "f64"). "f64": numbers ordered like Lucene's NumericUtils.doubleToSortableLong: -Infinity < ... < -0 < +0 < ... < +Infinity < NaN, every NaN one key. "i64": signed 64-bit integers given as bigint or integer numbers (BigInt64Array accepted); a value outside the int64 range or a fractional number is a RangeError. "ip": 1-d, IPv4 or IPv6 address strings stored as 16-byte keys in IPv6 order, an IPv4 address as its mapped form ::ffff:a.b.c.d (Lucene InetAddressPoint), so "1.2.3.4" and "::ffff:1.2.3.4" are one key; zone ids (%eth0) are a RangeError. "latlon": 2-d [lat, lon] in degrees as exact float64 (not Lucene's int32 quantization), latitude in [-90, 90] and longitude in [-180, 180], else RangeError. */
        type?: "f64" | "i64" | "ip" | "latlon";
        /** Dimensions per point, 1..4. Defaults to the row length for number[][] rows, else 1. A flat list must be a multiple of dims. */
        dims?: number;
        /** The uint32 document id of each point (default: the point's index). Ids may repeat (multi-valued documents). */
        docIds?: ArrayLike<number> | Uint32Array;
        /** Points per leaf block, 2..65536 (default 512). */
        leafSize?: number;
    }

    /** Immutable block k-d tree over 1..4-dimensional numeric points, IP addresses and lat/lon points (Lucene PointValues / BKD). The build splits on the
     *  widest dimension at the median into a balanced tree of leaves of at most leafSize points; inner split values live
     *  in one flat array. A query classifies each cell against the box: a cell inside the box adds all its doc ids in
     *  bulk with no per-point test, a crossing leaf tests each point, and a cell outside is skipped. Results are
     *  RoaringBitmap sets of doc ids. Bounds are inclusive in the type's key order; a bound with lo > hi on any
     *  dimension matches nothing. Queries run synchronously on the calling thread; `BKDTree.build` runs the build on the
     *  shared io pool and settles on the loop. */
    class BKDTree {
        /** Builds from number[] (1-d), number[][] rows, a flat array / typed array with `dims`, for "i64" a BigInt64Array or (bigint | number)[], for "ip" string[], for "latlon" [lat, lon][] or a flat [lat0, lon0, lat1, ...]. An empty list gives an empty tree. Synchronous: O(n log n) on the calling thread. */
        constructor(values: ArrayLike<number> | ArrayLike<ArrayLike<number>> | Float64Array | Float32Array | BigInt64Array | ArrayLike<bigint | number> | ArrayLike<ArrayLike<bigint | number>>, opts?: BKDTreeOptions);
        /** Same as the constructor, but the inputs are read on the loop and the O(n log n) build runs on the io pool; input errors reject the promise. */
        static build(values: ArrayLike<number> | ArrayLike<ArrayLike<number>> | Float64Array | Float32Array | BigInt64Array | ArrayLike<bigint | number> | ArrayLike<ArrayLike<bigint | number>>, opts?: BKDTreeOptions): Promise<BKDTree>;
        /** Doc ids of points with lo[d] <= p[d] <= hi[d] on every dimension (a box when dims > 1). For dims 1 the bounds may be scalars. For "i64", -Infinity / Infinity bounds mean the int64 extremes. */
        range(lo: number | bigint | ArrayLike<number | bigint>, hi: number | bigint | ArrayLike<number | bigint>): RoaringBitmap;
        /** The number of matching POINTS (not distinct doc ids) of range(lo, hi), without building the bitmap. */
        rangeCount(lo: number | bigint | ArrayLike<number | bigint>, hi: number | bigint | ArrayLike<number | bigint>): number;
        /** ("ip" trees) Doc ids of addresses inside the CIDR prefix, bounds inclusive: an IPv4 prefix a.b.c.d/n is the IPv6 range ::ffff:a.b.c.d/(96+n), so "0.0.0.0/0" matches every IPv4 address and no other IPv6 one, and "::/0" matches everything. Host bits are masked ("1.2.3.7/30" is 1.2.3.4..1.2.3.7). A non-string prefix is a TypeError; a malformed one (no /n, n above 32 or 128, a leading zero, a zone id) is a RangeError. Another tree type is a TypeError. */
        cidr(prefix: string): RoaringBitmap;
        /** ("latlon" trees) Doc ids of points with minLat <= lat <= maxLat and minLon <= lon <= maxLon, all inclusive. minLon > maxLon crosses the antimeridian (lon >= minLon or lon <= maxLon), as in Lucene LatLonPoint.newBoxQuery; minLat > maxLat matches nothing. Out-of-range degrees are a RangeError. */
        box(minLat: number, maxLat: number, minLon: number, maxLon: number): RoaringBitmap;
        /** ("latlon" trees) Doc ids of points within `meters` (finite, >= 0) of (lat, lon) by the haversine formula on a sphere of R = 6371008.7714 m: h = sin((p2-p1)/2)^2 + cos(p1) cos(p2) sin((l2-l1)/2)^2 with angles in radians (deg * PI / 180), d = 2 R asin(sqrt(min(1, h))), match when d <= meters. The formula is exact in float64 as written, so two spellings of one place (a pole at two longitudes, lon 180 and -180) are about 1e-9 m apart and radius 0 matches only the exact coordinates. Cells are pruned by the circle's bounding box (split at the antimeridian, all longitudes when it covers a pole); a cell is taken whole only when its four corners are within the radius and its longitudes lie within 90 degrees of the center, otherwise every point is checked. meters >= PI * R matches every point. */
        distance(lat: number, lon: number, meters: number): RoaringBitmap;
        /** The point count. */
        readonly size: number;
        /** The dimension count. */
        readonly dims: number;
        /** The value type. */
        readonly type: "f64" | "i64" | "ip" | "latlon";
        /** Points per leaf block. */
        readonly leafSize: number;
        /** The tree as a compact type-tagged record (type id 51): the built tree itself (split dims, split values, leaf-ordered keys and doc ids), so deserialize does not rebuild. */
        serialize(): Uint8Array;
        /** Rebuilds a tree from its record (record version 1; records written before "ip" and "latlon" existed load unchanged). The record is validated: header fields, split dims, every point inside its leaf cell and latlon degrees in range, so a forged record is a TypeError or a tree whose answers match a scan of its points. */
        static deserialize(bytes: Uint8Array | ArrayBuffer): BKDTree;
        /** Writes the structure to `path` atomically; see the module notes on FILES. */
        save(path: import("dyna:file").Path, opts?: PersistSaveOptions): PersistSaveResult;
        /** Reads a file written by save(); see the module notes on FILES for options and errors. */
        static load(path: import("dyna:file").Path, opts?: PersistLoadOptions): BKDTree;
    }

    /** Consistent-hash ring: replica r of node n sits at XXH64(UTF-8 "n#r", seed 0), a key at XXH64(key, 0); the owner is the
     *  first point at or after the key, wrapping (ties by name), so any language with xxHash64 can place keys identically. */
    class ConsistentHash {
        /** `replicas` (virtual nodes per unit of weight, 1..1000) defaults to 160. `cache` (default off): `jump` (or
         * `cache: true`) keeps a table indexed by the top bits of the key hash (about 8 ring points per bucket, 0.5 byte
         * per point) so get/getN search one bucket: about 1.4x faster at 10K points, 2x at 1M. A ring change marks the
         * table stale and the next lookup rebuilds it (O(points)), so a change followed by a lookup costs about 1.3-1.5x
         * more. Routing never changes; a loaded or deserialized ring starts with it off. Unknown keys inside `cache`
         * throw TypeError. */
        constructor(opts?: { replicas?: number; cache?: boolean | { jump?: boolean } });
        /** Rebuilds a ring from its record. */
        static deserialize(bytes: Uint8Array | ArrayBuffer): ConsistentHash;
        /** Reads a file written by save(); see the module notes on FILES for options and errors. */
        static load(path: import("dyna:file").Path, opts?: PersistLoadOptions): ConsistentHash;
        /** Jump consistent hash (Lamping & Veach 2014) of XXH64(key, 0): a bucket in 0..buckets-1 that moves minimally as buckets grow. */
        static jump(key: string, buckets: number): number;
        /** Adds a node or changes its weight (1..1000, default 1). Only this node's points are hashed and merged into the sorted ring: O(ring) per call, routing identical to a full rebuild; a change whose ring would pass 4,194,304 points throws and leaves the ring as it was. */
        addNode(name: string, weight?: number): this;
        /** Removes a node; false when it was absent. O(ring): its points are filtered out of the sorted ring, nothing is re-hashed. */
        removeNode(name: string): boolean;
        /** Whether the node is on the ring. */
        hasNode(name: string): boolean;
        /** The node owning the key, or undefined when the ring is empty. */
        get(key: string): string | undefined;
        /** Up to n distinct nodes clockwise from the key (replica placement). */
        getN(key: string, n: number): string[];
        /** The node names. */
        nodes(): string[];
        /** The node count. */
        readonly size: number;
        /** Virtual nodes per unit of weight. */
        readonly replicas: number;
        /** The ring point count. */
        readonly points: number;
        /** The contents as a compact type-tagged record. */
        serialize(): Uint8Array;
        /** Writes the structure to `path` atomically; see the module notes on FILES. */
        save(path: import("dyna:file").Path, opts?: PersistSaveOptions): PersistSaveResult;
    }

    /** BM25 full-text index over uint32 document ids; tokens are runs of letters, digits, `_` and non-ASCII. */
    class InvertedIndex {
        /** BM25 defaults: k1 1.2, b 0.75. Stop words go through the same tokenizer. */
        constructor(opts?: { k1?: number; b?: number; stopWords?: string[] });
        /** Rebuilds an index from its record. */
        static deserialize(bytes: Uint8Array | ArrayBuffer): InvertedIndex;
        /** Reads a file written by save(); see the module notes on FILES for options and errors. */
        static load(path: import("dyna:file").Path, opts?: PersistLoadOptions): InvertedIndex;
        /** The tokens the index would store for `text` (ASCII lower-cased, at most 128 bytes each). */
        static tokenize(text: string): string[];
        /** Indexes `text` under `id`, replacing an earlier document with that id. */
        add(id: number, text: string): this;
        /** Removes a document; false when it was absent. */
        remove(id: number): boolean;
        /** Whether the document is indexed. */
        has(id: number): boolean;
        /** The document's token count, or undefined. */
        docLength(id: number): number | undefined;
        /** [id, score] best first (ties by id). Defaults: mode "or", limit 10; `prefix` expands the last term. */
        search(query: string, opts?: { mode?: "and" | "or"; limit?: number; prefix?: boolean }): [number, number][];
        /** The number of documents containing the term. */
        docFreq(term: string): number;
        /** [term, docFreq] for indexed terms starting with `prefix`, most frequent first. Defaults: limit 10. */
        suggest(prefix: string, limit?: number): [string, number][];
        /** The document count. */
        readonly size: number;
        /** The number of terms that appear in some document. */
        readonly termCount: number;
        /** The mean document length in tokens. */
        readonly avgLength: number;
        /** The contents as a compact type-tagged record. */
        serialize(): Uint8Array;
        /** Writes the structure to `path` atomically; see the module notes on FILES. */
        save(path: import("dyna:file").Path, opts?: PersistSaveOptions): PersistSaveResult;
    }

    /** String interner: dense uint32 ids in first-seen order (dictionary encoding), strings kept in arena chunks. */
    class Interner {
        /** Creates an empty interner. */
        constructor();
        /** Rebuilds an interner from its record; ids are preserved. */
        static deserialize(bytes: Uint8Array | ArrayBuffer): Interner;
        /** Reads a file written by save(); see the module notes on FILES for options and errors. */
        static load(path: import("dyna:file").Path, opts?: PersistLoadOptions): Interner;
        /** The id for `s`, assigning the next id when new. */
        intern(s: string): number;
        /** The id for `s`, or undefined without assigning one. */
        lookup(s: string): number | undefined;
        /** Whether `s` has an id. */
        has(s: string): boolean;
        /** The string for an id, or undefined. */
        get(id: number): string | undefined;
        /** intern() for every string, in one call. */
        internMany(strings: string[]): Uint32Array;
        /** get() for every id; unknown ids give undefined. */
        getMany(ids: ArrayLike<number> | Uint32Array | Int32Array): (string | undefined)[];
        /** The number of distinct strings. */
        readonly size: number;
        /** Bytes of string storage. */
        readonly bytes: number;
        /** The contents as a compact type-tagged record. */
        serialize(): Uint8Array;
        /** Writes the structure to `path` atomically; see the module notes on FILES. */
        save(path: import("dyna:file").Path, opts?: PersistSaveOptions): PersistSaveResult;
    }
}

/** dyna:graph — integer-id graphs, edge types 0..31; paths, PageRank, analytics on CSR snapshots.
 *  Persists via serialize()/save()/load() like dyna:structures. */
declare module "dyna:graph" {
    type PersistSaveOptions = import("dyna:structures").PersistSaveOptions;
    type PersistLoadOptions = import("dyna:structures").PersistLoadOptions;
    type PersistSaveResult = import("dyna:structures").PersistSaveResult;

    /** Adjacency-list graph over integer node ids. Traversals and path methods run on a CSR snapshot taken at the call, so a
     *  visitor or heuristic that mutates the graph is reported (TypeError) while the walk itself stays on the snapshot; a graph
     *  with more than 2^32-1 stored arcs (an undirected edge stores two, a self loop one) is a RangeError for them.
     *  TYPED EDGES: every edge carries a type id, an integer 0..31 (an edge added without one is type 0; an undirected edge has its type on both arcs;
     *  parallel edges may differ in type). A GraphTypeMask selects types: bit t (mask & (1 << t)) admits type t, 0 admits none, -1 (or 0xFFFFFFFF) all.
     *  Traversals given `types` follow only admitted arcs (the source itself is always visited); without `types` they behave exactly as before.
     *  A graph whose edges are all type 0 stores, exports and serializes exactly as an untyped graph (no extra memory, same record bytes);
     *  once an edge of a nonzero type exists, exportCSR adds `types` and serialize() writes the typed record (one type byte per arc),
     *  which older builds reject as truncated. Option bags are strict (TypeError naming the valid keys). */
    class Graph {
        /** Creates a graph; `directed` and `weighted` are fixed at construction. */
        constructor(opts?: { directed?: boolean; weighted?: boolean });
        /** Rebuilds a graph from its record. */
        static deserialize(bytes: Uint8Array | ArrayBuffer): Graph;
        /** Reads a file written by save(); see the dyna:structures module notes on FILES for options and errors. */
        static load(path: import("dyna:file").Path, opts?: PersistLoadOptions): Graph;
        /** Builds a graph from parallel edge arrays in one call: edge i is src[i] -> dst[i] (both directions when undirected), added in index order, so
         *  the result equals that sequence of addEdge calls. `weights` needs `weighted: true`; `types` holds type ids 0..31; every given array has the
         *  same length (TypeError). nodeCount is max(opts.nodes, largest id + 1). A node id outside [0, 2^26), a non-finite weight or a type > 31 is a
         *  RangeError and builds nothing. `directed`/`weighted` must be booleans. */
        static fromEdges(src: Int32Array, dst: Int32Array, opts?: { directed?: boolean; weighted?: boolean; weights?: Float64Array; types?: Uint8Array; nodes?: number }): Graph;
        /** Adds a node and returns its id; a graph holds at most 2^26 nodes (RangeError past that). */
        addNode(): number;
        /** Adds an edge (both directions when undirected); nodes grow on demand up to 2^26 (a larger id is a RangeError); `w` is honoured only on a `{ weighted: true }` graph, else 1.0; `type` is the edge's type id, an integer 0..31 (default 0; RangeError outside, TypeError for a non-number). */
        addEdge(u: number, v: number, w?: number, type?: number): this;
        /** Targets of u's outgoing edges. */
        neighbors(u: number): number[];
        /** Writes u's out-neighbors into `out` and returns the degree; RangeError (nothing written) when `out` is too short. Never allocates. */
        neighborsInto(u: number, out: Int32Array): number;
        /** The adjacency as CSR in fresh arrays: edges[offsets[u] .. offsets[u+1]] are u's neighbors in insertion order (an undirected edge appears under both ends, a self loop once); `weights` (weighted graphs only) and `types` (only once an edge has a nonzero type) are aligned with `edges`. The result is a `GraphCSR`, so it can be handed straight to the `graph` functions. */
        exportCSR(): { offsets: Int32Array; edges: Int32Array; weights?: Float64Array; types?: Uint8Array; directed: boolean };
        /** False if `u` is out of range or no such edge exists. */
        hasEdge(u: number, v: number): boolean;
        /** The number of nodes. */
        readonly nodeCount: number;
        /** The number of `addEdge` calls (an undirected edge is one call). */
        readonly edgeCount: number;
        /** Node ids in breadth-first order from `src`. */
        bfs(src: number): number[];
        /** Node ids in depth-first order from `src`. */
        dfs(src: number): number[];
        /** BFS order over the arcs whose type `types` admits (the order of bfs(src) on the graph with the other arcs removed). The options must set visit and/or types. */
        bfs(src: number, opts: { types: GraphTypeMask }): number[];
        /** DFS order over the admitted arcs, as dfs(src) on the filtered graph. */
        dfs(src: number, opts: { types: GraphTypeMask }): number[];
        /** Visitor form: calls visit(node) in BFS order, stops when it returns true, returns the count visited; allocates nothing per node; `types` filters arcs. */
        bfs(src: number, opts: { visit: (node: number) => boolean | void; types?: GraphTypeMask }): number;
        /** Visitor form of dfs (smaller ids first); same contract as the bfs visitor. */
        dfs(src: number, opts: { visit: (node: number) => boolean | void; types?: GraphTypeMask }): number;
        /** Shortest distances; a single distance to dst when given. */
        dijkstra(src: number): number[];
        dijkstra(src: number, dst: number): number;
        /** Shortest distances from `src`; negative edge weights are allowed. */
        bellmanFord(src: number): number[];
        /** Node ids in topological order; a cycle throws RangeError. */
        topologicalSort(): number[];
        /** Visitor form: visits in topological order with early exit; a cycle still throws RangeError. */
        topologicalSort(opts: { visit: (node: number) => boolean | void }): number;
        /** Component id per node (weak components on a directed graph). */
        connectedComponents(): number[];
        /** All-pairs distance matrix; refused for n > 1024. */
        floydWarshall(): number[][];
        /** Minimum spanning forest. */
        mst(): { weight: number; edges: [number, number, number][] };
        /** A* search; closed nodes reopen on a shorter path, so an inconsistent heuristic stays optimal within a push budget of 64*n+1024 (RangeError beyond). */
        aStar(src: number, dst: number, heuristic: (node: number) => number): { dist: number; path: number[] };
        /** Dijkstra from `src` to `dst` with the path: `{ dist, path }` as aStar returns it (dist Infinity and an empty path when
         *  unreachable, [src] at 0 for src === dst). Parallel edges count at their lightest; a negative weight is a RangeError. The
         *  path follows shortestPathTree's predecessors. `types` restricts the search to admitted arcs (mask 0 leaves only src). */
        shortestPath(src: number, dst: number, opts?: { types?: GraphTypeMask }): { dist: number; path: number[] };
        /** The k-hop neighbourhood of `src` over admitted arcs (all when `types` is omitted): `nodes` in BFS order starting with src, `dist[i]` the hop
         *  count of nodes[i] (0 for src, at most k). k is an integer >= 0 (RangeError otherwise, TypeError for a non-number); src out of range is a RangeError. */
        neighborhood(src: number, k: number, opts?: { types?: GraphTypeMask }): { nodes: Int32Array; dist: Int32Array };
        /** Typed path pattern A-[hops[0]]->B-[hops[1]]->C...: the distinct nodes reached from `sources` by walks of exactly hops.length arcs whose i-th arc
         *  has a type admitted by hops[i], ascending. Walks may revisit nodes; an empty `hops` returns the distinct sources. O(hops.length * (n + m)).
         *  sources is a node id or an array of them (RangeError when out of range); at most 1024 hops (RangeError). */
        pathEndpoints(sources: number | ArrayLike<number>, hops: ArrayLike<GraphTypeMask>): Int32Array;
        /** Every walk matching the pattern of pathEndpoints, written as rows of hops.length + 1 node ids into `outNodes` and, when given, the hops.length arc
         *  types of the row into `outTypes`; returns the TOTAL number of matching walks. Order: sources as listed (a repeated source repeats its walks), then
         *  depth-first with each node's arcs in insertion order; parallel arcs that both match give separate rows (told apart by their types).
         *  Capacity: rows = outNodes.length / (hops.length + 1); when the total exceeds it, the first `rows` walks are written, the rest are only counted,
         *  and the total is still returned (grow and call again); slots past the written rows are untouched.
         *  TypeError: outNodes not an Int32Array or its length not a multiple of hops.length + 1, outTypes not a Uint8Array of exactly rows * hops.length,
         *  a detached or out-of-bounds view, an immutable buffer, outNodes and outTypes sharing memory. RangeError past 10^9 arc steps (enumeration budget).
         *  sources and hops are read before the views. */
        pathsInto(sources: number | ArrayLike<number>, hops: ArrayLike<GraphTypeMask>, outNodes: Int32Array, outTypes?: Uint8Array): number;
        /** Dijkstra's shortest-path tree from `src`: `dist[v]` (Infinity when unreachable) and `pred[v]`, the node before v on the
         *  path Dijkstra settled first (-1 for src and unreachable nodes; among equal-length paths, the first one relaxed). Distances
         *  equal dijkstra(src) and networkx.single_source_dijkstra; a negative weight is a RangeError. */
        shortestPathTree(src: number): { dist: Float64Array; pred: Int32Array };
        /** Longest (critical) path of a directed acyclic graph, networkx.dag_longest_path semantics: unweighted edges count 1,
         *  parallel edges at their heaviest, and a path restarts at v when its best incoming sum is negative, so `length` >= 0 and
         *  `dist[v]` is the longest path ending at v (never below 0). Ties are deterministic: the smallest predecessor id, then the
         *  smallest end node; an empty graph gives length 0 and an empty path. A cycle (a self loop included) is a RangeError, an
         *  undirected graph a TypeError. O(n + m). */
        criticalPath(): { length: number; path: number[]; dist: Float64Array };
        /** PageRank by pull power iteration (networkx.pagerank semantics: dangling mass and teleport follow the personalization vector, stop when the L1 change < n * tolerance). Weighted graphs use edge weights (negative rejects); parallel edges add up. Defaults damping 0.85, tolerance 1e-6, maxIterations 100; RangeError "no convergence within N iterations" when the limit is hit. See the Analytics note on `graph`. */
        pageRank(opts?: GraphPageRankOptions): Promise<Float64Array>;
        /** Topic-sensitive personalized PageRank: one PageRank per topic, all topics in one pass (each power-iteration sweep over the arcs updates every
         *  topic). Topic t's personalization puts weight on its seeds (uniform, or `weights` normalized; a repeated seed adds up). Per topic the iteration is
         *  pageRank's (networkx.pagerank semantics): start 1/n, x' = damping * (pull over arcs + dangling mass * p) + (1 - damping) * p, where dangling mass (nodes
         *  with no admitted out-weight) follows the topic's personalization; stop when the L1 change < n * tolerance and return that iterate.
         *  Arcs: only types admitted by `types` (default all), weight = edge weight (1 unweighted) * typeWeights[type] (default 1); a weight of 0 removes the arc
         *  from the out-weight, so typeWeights[t] = 0 equals clearing bit t. Each topic stops at its own convergence, so a topic's row is identical whether it
         *  runs alone or in a batch. Resolves to a Float64Array of topics.length * nodeCount entries, row-major: result[t * nodeCount + v]; [] topics gives an
         *  empty array. Rejects with RangeError "topic T: no convergence within N iterations" when any topic misses maxIterations (defaults damping 0.85,
         *  tolerance 1e-6, maxIterations 100, as pageRank). Topics, options and the graph are read when called (later mutations do not affect the run); large
         *  runs ((n + m) * 16 * topics >= 2^16) compute off the JS thread. RangeError: a seed out of range, no seeds, a negative or non-finite seed weight or
         *  type weight, a zero seed-weight sum, a negative edge weight, more than 65536 topics or topics * nodes > 2^27; TypeError: a topic that is neither
         *  an array of seeds nor { seeds, weights? }, unknown keys in either bag. */
        topicPageRank(topics: GraphTopic[], opts?: GraphTopicPageRankOptions): Promise<Float64Array>;
        /** Asynchronous label propagation on the underlying undirected graph (weighted votes on weighted graphs), seeded shuffle each round, ties broken by the seeded generator; stops when every node holds a most frequent neighbour label or after maxRounds (default 100). Labels are compacted to 0..k-1 by first appearance. */
        labelPropagation(opts?: { seed?: number; maxRounds?: number }): Promise<Int32Array>;
        /** Louvain community detection (networkx.louvain_communities semantics: level stops when modularity gains <= threshold) on the underlying undirected graph; a directed graph is clustered on w(u,v) + w(v,u). Defaults resolution 1, threshold 1e-7, seed 0 (deterministic for a seed). `modularity` is that of `communities` on the input graph. */
        louvain(opts?: { resolution?: number; threshold?: number; seed?: number }): Promise<{ communities: Int32Array; count: number; modularity: number; levels: number }>;
        /** Newman modularity of a partition (ids 0..n-1, one per node): undirected Q = sum_c L_c/m - resolution*(d_c/2m)^2, directed Q = sum_c L_c/m - resolution*out_c*in_c/m^2; a self loop counts once in m and twice in a degree, parallel edges add; an edgeless graph scores 0. */
        modularity(communities: ArrayLike<number>, opts?: { resolution?: number }): Promise<number>;
        /** k-core number of every node (Batagelj-Zaversnik bucket peeling, O(n + m)) on the underlying undirected simple graph: self loops and parallel edges are ignored, a directed graph's arcs count once per neighbour pair. */
        coreNumbers(): Promise<Int32Array>;
        /** Triangles through each node and in total, on the underlying undirected simple graph (degree-ordered orientation with sorted-merge/galloping intersections, multi-threaded). */
        triangles(): Promise<{ total: number; perVertex: Float64Array }>;
        /** Local clustering coefficient per node, its mean over all nodes (networkx.average_clustering, 0 for the empty graph) and transitivity (3 * triangles / connected triples), on the underlying undirected simple graph. */
        clustering(): Promise<{ local: Float64Array; average: number; transitivity: number }>;
        /** Brandes betweenness (networkx.betweenness_centrality semantics, normalization included), over shortest paths of the graph with self loops removed and parallel edges merged (minimum weight). `weighted` defaults to whether the graph is weighted and then needs every weight > 0. `samples: k` uses k distinct seeded sources (k < n rescales like networkx's k=), `sources` names them; independent sources run on all cores and the result is bit-identical for any thread count. */
        betweenness(opts?: GraphBetweennessOptions): Promise<Float64Array>;
        /** Closeness centrality from distances TO each node (incoming on a directed graph), Wasserman-Faust scaled by default (networkx.closeness_centrality); `weighted` as for betweenness (weights >= 0). One BFS/Dijkstra per node, multi-threaded. */
        closeness(opts?: { wfImproved?: boolean; weighted?: boolean }): Promise<Float64Array>;
        /** Strongly connected components by iterative Tarjan: ids are a reverse topological order of the condensation (no arc goes from a lower to a higher id; sinks first). On an undirected graph they equal the connected components. */
        stronglyConnectedComponents(): Promise<{ count: number; labels: Int32Array }>;
        /** Weakly connected components by union-find; ids are numbered in order of each component's smallest node (connectedComponents' numbering). */
        weaklyConnectedComponents(): Promise<{ count: number; labels: Int32Array }>;
        /** Topological generation of every node of a directed acyclic graph: 0 for sources, else 1 + the largest level of a predecessor (networkx.topological_generations); `count` is the number of levels. A cycle rejects with RangeError, an undirected graph with TypeError. */
        topologicalLevels(): Promise<{ count: number; labels: Int32Array }>;
        /** Maximum flow from `source` to `sink` by Dinic's algorithm; capacities are the edge weights (1 on an unweighted graph, negative rejects), an undirected edge carries flow either way, parallel edges add. `flows` is the flow on each arc in exportCSR order, `sourceSide` marks the source side of a minimum cut. */
        maxFlow(source: number, sink: number): Promise<{ value: number; flows: Float64Array; sourceSide: Uint8Array }>;
        /** Maximum-cardinality bipartite matching by Hopcroft-Karp on the underlying undirected simple graph; `left` gives the sides (truthy = left), otherwise they come from a BFS 2-colouring. `mate[v]` is v's partner or -1. A graph that is not bipartite (under `left`) rejects with RangeError. */
        bipartiteMatching(opts?: { left?: ArrayLike<number | boolean> }): Promise<{ size: number; mate: Int32Array }>;
        /** Link-prediction scores for node pairs ([[u, v], ...] or a flat [u0, v0, u1, v1, ...] array) on the underlying undirected simple graph, as networkx defines them: jaccard (default) |N(u)&N(v)| / |N(u)|N(v)|, adamicAdar sum 1/ln deg(w), resourceAllocation sum 1/deg(w), commonNeighbors, preferentialAttachment deg(u)*deg(v); common neighbours exclude u and v. */
        linkPrediction(pairs: ArrayLike<number> | [number, number][], opts?: { method?: GraphLinkMethod }): Promise<Float64Array>;
        /** The graph as a compact type-tagged record with delta-varint encoded adjacency. */
        serialize(): Uint8Array;
        /** Writes the structure to `path` atomically; see the dyna:structures module notes on FILES. */
        save(path: import("dyna:file").Path, opts?: PersistSaveOptions): PersistSaveResult;
    }

    /** A set of edge types as a bitmask: bit t admits type t (0..31); an integer in [-2^31, 2^32-1] taken modulo 2^32, so 1 << 31 and -1 work (RangeError otherwise, TypeError for a non-number). */
    type GraphTypeMask = number;
    /** Options of Graph#pageRank / graph.pageRank. */
    interface GraphPageRankOptions {
        /** Probability of following an edge, in [0, 1]; default 0.85. */
        damping?: number;
        /** Stop when the L1 change of an iteration is below n * tolerance; default 1e-6. */
        tolerance?: number;
        /** Iteration cap, >= 1; default 100. */
        maxIterations?: number;
        /** Teleport (and dangling) distribution, one non-negative entry per node, normalized internally; personalized PageRank. */
        personalization?: Float64Array | number[];
    }
    /** A topicPageRank topic: its seed nodes (an array or integer typed array), or seeds with one non-negative weight each. */
    type GraphTopic = ArrayLike<number> | { seeds: ArrayLike<number>; weights?: Float64Array | number[] };
    /** Options of Graph#topicPageRank. */
    interface GraphTopicPageRankOptions {
        /** Probability of following an arc, in [0, 1]; default 0.85. */
        damping?: number;
        /** Stop a topic when its L1 change is below n * tolerance; default 1e-6. */
        tolerance?: number;
        /** Iteration cap per topic, >= 1; default 100. */
        maxIterations?: number;
        /** Edge types the walk may follow (default all). */
        types?: GraphTypeMask;
        /** 32 finite non-negative multipliers, one per edge type, applied to every arc's weight. */
        typeWeights?: Float64Array | number[];
    }
    /** Options of Graph#betweenness / graph.betweenness. */
    interface GraphBetweennessOptions {
        /** Divide by the number of pairs (default true), as networkx does. */
        normalized?: boolean;
        /** Count the path endpoints too (default false). */
        endpoints?: boolean;
        /** Estimate from this many distinct sources drawn with `seed` (1..n). */
        samples?: number;
        /** Seed of the source sample; default 0. */
        seed?: number;
        /** The exact distinct source nodes to accumulate from (instead of `samples`). */
        sources?: ArrayLike<number>;
        /** Shortest paths by edge weight; defaults to whether the graph has weights. */
        weighted?: boolean;
    }
    /** A link-prediction score. */
    type GraphLinkMethod = "jaccard" | "adamicAdar" | "resourceAllocation" | "commonNeighbors" | "preferentialAttachment";
    /** A graph as compressed sparse rows: node u's out-arcs are edges[offsets[u] .. offsets[u+1]] (n + 1 offsets starting at 0, never decreasing, ending at edges.length; targets < n; at most 2^26 nodes). `weights` (finite) aligns with `edges`; `directed` defaults to true -- pass false when every undirected edge is stored under both ends (Graph#exportCSR output is accepted as is). */
    interface GraphCSR {
        offsets: Int32Array | Uint32Array;
        edges: Int32Array | Uint32Array;
        weights?: Float64Array | number[];
        directed?: boolean;
    }

    /** Graph analytics over CSR arrays: the same kernels as the Graph methods of the same names, for graphs held as typed arrays (Graph#exportCSR, CSRGraph-style builders, files).
     *  Analytics note (Graph methods and these functions alike): every call returns a Promise. Arguments are read and validated and the graph is SNAPSHOTTED on the calling thread when the call starts (mutating the Graph or the arrays afterwards cannot change the result); bad arguments REJECT the promise, never throw synchronously. A call whose work is large (about 64K node+edge visits) runs on a thread created for that one call, as dyna:ml does, so the event loop keeps running; inside it PageRank, betweenness, closeness, triangles and link prediction split their work over all cores, with results bit-identical for any thread count. Results are fresh typed arrays indexed by node id. Runtime shutdown interrupts a running call (InternalError "interrupted"). */
    namespace graph {
        function pageRank(csr: GraphCSR, opts?: GraphPageRankOptions): Promise<Float64Array>;
        function labelPropagation(csr: GraphCSR, opts?: { seed?: number; maxRounds?: number }): Promise<Int32Array>;
        function louvain(csr: GraphCSR, opts?: { resolution?: number; threshold?: number; seed?: number }): Promise<{ communities: Int32Array; count: number; modularity: number; levels: number }>;
        function modularity(csr: GraphCSR, communities: ArrayLike<number>, opts?: { resolution?: number }): Promise<number>;
        function coreNumbers(csr: GraphCSR): Promise<Int32Array>;
        function triangles(csr: GraphCSR): Promise<{ total: number; perVertex: Float64Array }>;
        function clustering(csr: GraphCSR): Promise<{ local: Float64Array; average: number; transitivity: number }>;
        function betweenness(csr: GraphCSR, opts?: GraphBetweennessOptions): Promise<Float64Array>;
        function closeness(csr: GraphCSR, opts?: { wfImproved?: boolean; weighted?: boolean }): Promise<Float64Array>;
        function stronglyConnectedComponents(csr: GraphCSR): Promise<{ count: number; labels: Int32Array }>;
        function weaklyConnectedComponents(csr: GraphCSR): Promise<{ count: number; labels: Int32Array }>;
        /** A `directed: false` CSR rejects with TypeError. */
        function topologicalLevels(csr: GraphCSR): Promise<{ count: number; labels: Int32Array }>;
        /** `flows` follows the order of `csr.edges`. */
        function maxFlow(csr: GraphCSR, source: number, sink: number): Promise<{ value: number; flows: Float64Array; sourceSide: Uint8Array }>;
        function bipartiteMatching(csr: GraphCSR, opts?: { left?: ArrayLike<number | boolean> }): Promise<{ size: number; mate: Int32Array }>;
        function linkPrediction(csr: GraphCSR, pairs: ArrayLike<number> | [number, number][], opts?: { method?: GraphLinkMethod }): Promise<Float64Array>;
    }
}

/** dyna:sys — the OS boundary: environment, argv, process identity and limits, CPU/memory facts, Exec (blocking) and Spawn (async) subprocesses without a shell. */
declare module "dyna:sys" {
    /** A snapshot object of the current environment. */
    function env(): Record<string, string>;
    /** A NUL-bearing name is refused (no environment name can contain one). */
    function getEnv(name: string): string | undefined;
    /** Overwrites an entry; refuses empty names, `=` in a name, or NUL. NOT synchronized with worker/HTTP/pool threads: mutate before starting them. */
    function setEnv(name: string, value: string): void;
    /** The process argument vector (argv[0] first). */
    function args(): string[];
    /** Current working directory; grows past `PATH_MAX` rather than truncating. */
    function cwd(): string;
    /** A NUL-bearing path is refused rather than chdir'd by its prefix. */
    function chDir(path: string): void;
    /** "darwin", "linux", or the lowercased uname sysname ("freebsd", ...); "unknown" only when uname() fails. */
    function platform(): string;
    /** The uname machine name, normalised: aarch64 -> "arm64", amd64 -> "x86_64"; others pass through lowercased. */
    function arch(): string;
    /** The five utsname fields as strings; nodename matches hostName(). */
    function uname(): { sysname: string; nodename: string; release: string; version: string; machine: string };
    /** The real user id. */
    function getuid(): number;
    /** The real group id. */
    function getgid(): number;
    /** setuid(2); without privilege throws an Error with code "EPERM"; id must be a non-negative integer. */
    function setUid(id: number): void;
    /** setgid(2); same discipline as setUid. */
    function setGid(id: number): void;
    /** CPU time of THIS process as {user, system} in fractional seconds (getrusage). */
    function cpuUsage(): { user: number; system: number };
    /** CPU time of the CALLING thread in fractional milliseconds (clock_gettime CLOCK_THREAD_CPUTIME_ID, nanosecond resolution on Linux and macOS): time a descheduled thread does not accrue, and helper threads are not counted. One call costs about 0.15 us on macOS, against about 0.4 us plus an object for cpuUsage(). Use it to time a function under a loaded host. */
    function threadCpuNow(): number;
    /** The full getrusage(RUSAGE_SELF) struct; user/system in seconds, maxrss normalised to BYTES; the fault/switch counters are kernel-specific. */
    /** Process-wide counters of the fork-join helper threads some natives use inside one call (Argon2id lanes, BLAKE3 over 128 KiB, lz4Frame over one 4 MiB block, DataFrame radix sorts from 98,304 rows): runs, serial runs, helpers spawned, spawn failures, helpers live now, the high-water mark, and the helper budget (CPUs - 1). `--io-threads 0` makes every such call serial. */
    function parStats(): { runs: number; serialRuns: number; spawned: number; spawnFailures: number; live: number; peak: number; budget: number };
    function rusage(): { user: number; system: number; maxrss: number; idrss: number; isrss: number; minflt: number; majflt: number; nswap: number; inblock: number; oublock: number; msgsnd: number; msgrcv: number; nsigs: number; nvcsw: number; nivcsw: number };
    /** The process id. */
    function pid(): number;
    /** The host name. */
    function hostName(): string;
    /** `$HOME`, else the passwd entry. */
    function homeDir(): string;
    /** {model, cores?, threads, mhz?, features} of the selected SIMD dispatch. */
    function cpuInfo(): { model: string; cores?: number; threads: number; mhz?: number; features: string[] };
    /** System memory in bytes. */
    function memInfo(): { total: number; free: number; available: number };
    /** [1, 5, 15]-minute load averages. */
    function loadAvg(): [number, number, number];
    /** Seconds since boot. */
    function uptime(): number;
    /** Volume statistics for the filesystem CONTAINING path. */
    function diskUsage(path: string): { total: number; free: number; available: number };
    /** The engine's allocation counters, OS peakRss, and the native ledger: nativeSize (libc memory held by dyna:* modules; dyna:structures and other library-backed allocations count only while a limit is set) and nativeLimit (the cap in force, 0 = none). */
    function memoryUsage(): { mallocCount: number; mallocSize: number; memoryUsedCount: number; memoryUsedSize: number; objCount: number; objSize: number; strCount: number; strSize: number; propCount: number; shapeCount: number; arrayCount: number; peakRss: number; nativeSize: number; nativeLimit: number };
    /** Caps module-native memory at `bytes` (0 = uncapped); allocation past it throws out-of-memory. RRule and the Temporal classes are accounted like every other native class.
     *  Under an operator ceiling (--native-memory-limit) a script may only LOWER the cap; memory owned by linked libraries (OpenSSL, SQLite) is not counted.
     *  A typed-array result a module computes natively (Graph analytics and exportCSR weights, CsvIndex.column, mathx stats/geo arrays, DataFrame columns) moves off the native ledger when it is returned: it counts toward the engine heap (mallocSize, the engine memory limit) for its lifetime, never toward nativeSize, so collecting it never leaves the ledger high. */
    function setNativeMemoryLimit(bytes: number): void;

    /** Result of Exec, the canonical way to run a program: argv only, no shell, NUL-bearing strings refused (os.exec is the --std legacy form). */
    interface ExecResult {
        code: number | null;
        signal: string | null;
        stdout: string | Uint8Array;
        stderr: string | Uint8Array;
        timedOut: boolean;
    }
    /** Options for Exec. */
    interface ExecOptions {
        cwd?: string;
        /** REPLACES the child's environment entirely (execve semantics), unless extendEnv is true. */
        env?: Record<string, string>;
        /** true: the child gets this process's environment with `env` laid over it (an `env` key wins; PATH and HOME survive), e.g. `Exec("git", ["log"], { env: { GIT_PAGER: "cat" }, extendEnv: true })`. Default false. Must be a boolean (TypeError otherwise). */
        extendEnv?: boolean;
        input?: BytesInput;
        timeoutMs?: number;
        /** Per-stream stdout/stderr cap in bytes (default 8 MiB); crossing it kills the child and throws a RangeError naming the stream.
         *  Big outputs (a whole-tree `git diff`, `git show` of a large file) pass it: `Exec("git", ["diff"], { maxBuffer: 256 << 20 })`; unbounded output belongs in Spawn, which streams. */
        maxBuffer?: number;
        encoding?: "utf8" | "bytes";
        /** Drops the child's credentials after chdir (gid before uid); non-negative integers. */
        uid?: number;
        gid?: number;
    }
    /** Runs a program to completion, BLOCKING the thread, and returns its ExecResult; throws when it cannot spawn.
     *  Child lifetime (Exec and Spawn alike): every child runs in its own session and process group (setsid), so a group signal reaches its
     *  descendants, and this process tracks each live child group until it reaps it. When this process ends it takes them down: on a normal
     *  exit and on an uncaught-error exit (atexit), and on SIGTERM, SIGINT or SIGHUP (a handler installed at the first child, only over the
     *  default disposition: a signal that is ignored, or that os.signal handles, is left alone), it sends each group SIGTERM, waits up to 2 s
     *  reaping, then SIGKILLs what is left, and a signal is then re-raised so this process still dies BY it. The --timeout-ms hard stop
     *  (exit 113) SIGKILLs the groups at once. Not covered: SIGKILL of this process, abort(), and children beyond 4096 live at once.
     *  There is no detached mode: a child meant to outlive its parent needs a daemonizing wrapper of its own. */
    function Exec(command: string, args?: string[], options?: ExecOptions): ExecResult;
    /** Resolves a program name against PATH; null when not found. */
    function Which(name: string): string | null;

    /* ---- Spawn: the async child with streaming stdio ------------ */

    /** One output pipe in ByteSource shape (read(buf) resolves a count, 0 = EOF). Buffered up to `maxPipe`, then the child BLOCKS: true backpressure. One read at a time.
     *  dyna:stream consumes it directly: `for await (const line of lines(p.stdout))`, `ndjson(p.stdout)`, `toReadableStream(p.stdout).pipeTo(toFile(path))`. */
    interface SpawnPipe {
        read(buf: Uint8Array): Promise<number>;
        /** `for await (const chunk of p.stdout)`: each chunk is a fresh Uint8Array copy (at most 64 KiB) in arrival order; the loop ends at EOF; `break` (or return/throw out of the loop) closes the pipe. */
        [Symbol.asyncIterator](): AsyncIterableIterator<Uint8Array>;
        /** Stop draining. A pending read() is rejected, not left hanging; bytes already buffered are dropped, so a later read() resolves 0 (EOF), also after a for-await `break`. */
        close(): void;
        dispose(): void;
        [Symbol.dispose](): void;
    }

    /** The child's stdin as a sink (write/flush/close), present when stdin is a pipe; the queue is bounded by maxPipe, and EPIPE rejects flush and later writes. */
    interface SpawnStdin {
        write(buf: BytesInput): Promise<number>;
        flush(): Promise<void>;
        close(): void;
        dispose(): void;
        [Symbol.dispose](): void;
    }

    /** Options for Spawn. */
    interface SpawnOptions {
        cwd?: string;
        /** Replaces the child's environment, unless extendEnv is true (same rules as ExecOptions). */
        env?: Record<string, string>;
        /** true: this process's environment with `env` laid over it; default false. */
        extendEnv?: boolean;
        /** Written to the child's stdin, then EOF (implies stdin: "pipe"). */
        input?: BytesInput;
        /** "ignore" (default: /dev/null, the child reads EOF), "pipe" (p.stdin becomes writable), or "inherit". */
        stdin?: "pipe" | "ignore" | "inherit";
        /** SIGTERM at the deadline, SIGKILL after a 2 s grace; wait() reports timedOut. Pending reads settle at EOF even if a grandchild inherited the pipe. */
        timeoutMs?: number;
        /** Per-pipe buffer bound where backpressure starts (default 8 MiB, at most 2^30). */
        maxPipe?: number;
        /** Same discipline as Exec's uid/gid. */
        uid?: number;
        gid?: number;
        /** Whether empty PATH elements mean the cwd (execvp behaviour). Default true. */
        allowPathCwd?: boolean;
    }

    /** What Spawn.wait() resolves with. */
    interface SpawnResult {
        /** Exit code, or null when a signal killed the child (Exec's rule). */
        code: number | null;
        /** "SIGTERM", "SIGKILL", ... or null for a normal exit. */
        signal: string | null;
        /** True when opts.timeoutMs fired. */
        timedOut: boolean;
    }

    /** The async child process: argv only, no shell, nothing blocks the JS thread; stdout/stderr are ByteSource-shaped pipes on the shared reactor.
     *  A dropped or closed Spawn SIGKILLs and reaps the child GROUP; pending reads and wait() still settle. No zombies.
     *  A read(), flush() or wait() still parked when the engine shuts down (an uncaught error ends the script) is rejected with "... aborted at
     *  engine shutdown", so the process exits with the script's error, never an abort. The child's lifetime against this process: see Exec. */
    class Spawn {
        constructor(command: string, args?: string[], options?: SpawnOptions);
        /** The child's pid; -1 once closed. */
        get pid(): number;
        /** Asks the kernel right now (WNOHANG), not just the last tick. */
        get exited(): boolean;
        get closed(): boolean;
        readonly stdout: SpawnPipe;
        readonly stderr: SpawnPipe;
        readonly stdin: SpawnStdin;
        /** Resolves {code, signal, timedOut} when the child exits; one wait() at a time; replayed after exit. */
        wait(): Promise<SpawnResult>;
        /** Signals the child GROUP with a number (1..31) or name (default SIGTERM); true when the child was still running. After TERM/KILL/INT/HUP/QUIT, reads reach EOF once the child is reaped even if a surviving descendant holds the pipe. */
        kill(signal?: number | string): boolean;
        /** Deterministic teardown: SIGKILL the group, reap, release everything, settle a parked wait(). */
        close(): void;
        dispose(): void;
        [Symbol.dispose](): void;
    }
}

/** dyna:time — clocks, RFC 3339 and Go-style layouts, durations, civil types (PlainDate/PlainTime/PlainDateTime), time zones, RFC 5545 recurrence (RRule). */
declare module "dyna:time" {
    // POSIX only. Clocks: now()/nowSec()/nowMillis()/nowUnixNano() read the WALL clock; monotonicNano() is MONOTONIC — use it to measure durations.

    /** Go-style duration units in NANOSECONDS (Second === 1e9): multiply to build the values parseDuration returns. */
    const Nanosecond: number;
    const Microsecond: number;
    const Millisecond: number;
    const Second: number;
    const Minute: number;
    const Hour: number;

    /** Parses "300ms", "-1.5h", "2h45m" into a count of NANOSECONDS: a `number` below 2^53 ns, at or above 2^53 ns a `bigint`. Units ns/us/ms/s/m/h; "d" is NOT accepted. */
    function parseDuration(str: string): number | bigint;
    /** parseDuration in MILLISECONDS, always a `number` (the correctly-rounded double of ns/1e6). */
    function parseDurationMs(str: string): number;
    /** parseDuration in SECONDS, always a `number`. */
    function parseDurationSecs(str: string): number;
    /** The inverse of parseDuration; 0 is "0s" ("2h45m" round-trips as "2h45m0s"). */
    function durationString(ns: number | bigint): string;

    /** A calendar duration; years fold into months and weeks into days. */
    class Duration {
        /** Components are limited to |value| <= 1e12 (RangeError beyond; keeps downstream int64 folds in range). */
        constructor(opts: { years?: number; months?: number; weeks?: number; days?: number; hours?: number; minutes?: number; seconds?: number; milliseconds?: number });
        readonly years: number;
        readonly months: number;
        /** The day count. */
        readonly days: number;
        /** 1, -1 or 0. */
        readonly sign: number;
        /** True when every component is zero. */
        readonly blank: boolean;
        /** ISO 8601; a mixed-sign value throws. */
        toString(): string;
    }

    /** `new Duration({ milliseconds: ms })` as a call (non-integers truncate toward zero). */
    function durationMs(ms: number | bigint): Duration;
    /** `new Duration({ seconds: s })` as a call; fractional input truncates to whole seconds FIRST (use durationMs for sub-second precision). */
    function durationSecs(s: number | bigint): Duration;

    /** {sec, nsec} from CLOCK_REALTIME (wall time). */
    function now(): { sec: number; nsec: number };
    /** Whole seconds since the Unix epoch (wall time) -- now().sec as one number. */
    function nowSec(): number;
    /** BigInt nanoseconds since the Unix epoch (wall time); int64, so it ends in 2262. */
    function nowUnixNano(): bigint;
    /** Alias of nowUnixNano: the same clock under the duration-side name. */
    function nowNanos(): bigint;
    /** Milliseconds since the Unix epoch (wall time), like Date.now(). */
    function nowMillis(): number;
    /** BigInt nanoseconds from CLOCK_MONOTONIC (arbitrary origin): durations only, never compared across processes. */
    function monotonicNano(): bigint;

    /** RFC 3339 text for unix `sec`: the legacy tail (nsec, utc), or a strict bag { nsec, offsetMinutes } rendering at a fixed offset (-1439..1439). Slots are typed, never coerced. */
    function formatRFC3339(sec: number, nsec?: number, utc?: boolean): string;
    function formatRFC3339(sec: number, opts: { nsec?: number; offsetMinutes?: number }): string;
    /** Formats with Go-style layout tokens (2006 January Jan Monday Mon 01 02 15 04 05) plus %z; as in Go, the long names win where they match ('Monday' is the full weekday, never 'Mon' + 'day'); Format.parse matches names case-sensitively; { offsetMinutes } fixes the offset (default UTC). */
    function formatUnix(sec: number, layout: string, opts?: { offsetMinutes?: number }): string;
    /** Strict RFC 3339 parse; returns {sec, nsec}. */
    function parseRFC3339(str: string): { sec: number; nsec: number };
    /** Unix seconds (UTC) from civil fields; an out-of-range month carries; |day| <= 1e9 and |hour/minute/second| <= 1e6, else RangeError. */
    function date(y: number, mo: number, d: number, h?: number, mi?: number, s?: number): number;
    /** {year, month, day, hour, min, sec, weekday, yday}; weekday 0 = Sunday. */
    function fromUnix(sec: number): { year: number; month: number; day: number; hour: number; min: number; sec: number; weekday: number; yday: number };

    /** A compiled Go-style layout; { offsetMinutes } fixes the offset for format() and for parse() unless the input's own %z supplies one. */
    class Format {
        constructor(layout: string, opts?: { offsetMinutes?: number });
        /** Formats unix seconds with this layout. */
        format(sec: number): string;
        /** The strict inverse; omitted fields default to 1970-01-01T00:00:00Z; an unsupported year is a SyntaxError. */
        parse(str: string): number;
        /** The original layout. */
        readonly layout: string;
    }

    /** An immutable calendar date, proleptic Gregorian. */
    class PlainDate {
        /** All three are required. An impossible date is refused, never rolled over: `31 February` throws. */
        constructor(year: number, month: number, day: number);
        readonly year: number;
        readonly month: number;
        readonly day: number;
        /** ISO 8601, Monday is 1. */
        readonly dayOfWeek: number;
        readonly dayOfYear: number;
        readonly daysInMonth: number;
        readonly daysInYear: number;
        readonly inLeapYear: boolean;
        /** Days since 1970-01-01. */
        readonly epochDay: number;
        /** Months move first and clamp, then days are added exactly. */
        add(duration: Duration): PlainDate;
        subtract(duration: Duration): PlainDate;
        /** A Duration of whole months plus the remaining days. */
        until(other: PlainDate): Duration;
        /** `-1`, `0` or `1`. */
        compare(other: PlainDate): -1 | 0 | 1;
        /** ISO 8601; years outside 0..9999 print signed with six digits. */
        toString(): string;
    }

    /** A date and time of day with no zone; adding time carries into the date. */
    class PlainDateTime {
        constructor(year: number, month: number, day: number, hour?: number, minute?: number, second?: number, millisecond?: number);
        readonly year: number;
        readonly month: number;
        readonly day: number;
        readonly hour: number;
        readonly minute: number;
        readonly second: number;
        readonly millisecond: number;
        readonly epochDay: number;
        readonly dayOfWeek: number;
        add(duration: Duration): PlainDateTime;
        subtract(duration: Duration): PlainDateTime;
        /** The Duration from this to `other`: whole months first, then days, then the time remainder in ms, so a.add(a.until(b)) equals b exactly. */
        until(other: PlainDateTime): Duration;
        toPlainDate(): PlainDate;
        toPlainTime(): PlainTime;
        /** `-1`, `0` or `1`. */
        compare(other: PlainDateTime): -1 | 0 | 1;
        /** ISO 8601 with `T`, milliseconds omitted when zero. */
        toString(): string;
    }

    /** A wall-clock time of day; one integer millisecond count since midnight. */
    class PlainTime {
        /** `24:00` is refused (it names the same wall clock as `00:00`). */
        constructor(hour?: number, minute?: number, second?: number, millisecond?: number);
        readonly hour: number;
        readonly minute: number;
        readonly second: number;
        readonly millisecond: number;
        /** The whole value. */
        readonly msSinceMidnight: number;
        /** Adds the time part, wrapping at midnight; a duration carrying MONTHS is a RangeError, weeks/days are ignored. */
        add(duration: Duration): PlainTime;
        subtract(duration: Duration): PlainTime;
        /** `-1`, `0` or `1`. */
        compare(other: PlainTime): -1 | 0 | 1;
        /** `"HH:MM:SS"`, milliseconds appended when non-zero. */
        toString(): string;
    }

    /** RFC 5545 recurrence rules, UTC whole-second unix time. */
    interface RRuleOptions {
        freq: "YEARLY" | "MONTHLY" | "WEEKLY" | "DAILY" | "HOURLY" | "MINUTELY" | "SECONDLY";
        interval?: number;
        /** count and until are MUTUALLY EXCLUSIVE (RFC 5545): both set is a TypeError (options and string parsers alike). */
        count?: number;
        until?: Date | string | number;
        dtstart?: Date | string | number;
        wkst?: number | "MO" | "TU" | "WE" | "TH" | "FR" | "SA" | "SU";
        bymonth?: number[];
        bymonthday?: number[];
        byyearday?: number[];
        byweekno?: number[];
        bysetpos?: number[];
        byweekday?: (string | number)[];
    }
    /** An RFC 5545 recurrence rule, expanded into Dates on demand. */
    class RRule {
        /** `BYHOUR`/`BYMINUTE`/`BYSECOND`/`BYEASTER` are refused rather than silently ignored. */
        constructor(opts: RRuleOptions);
        /** Parses "RRULE:FREQ=..." parts plus optional DTSTART: lines; `opts.dtstart` supplies the start otherwise. */
    static fromString(str: string, opts?: { dtstart?: Date | string | number }): RRule;
        /** Every occurrence as Dates (`limit` caps the count; an uncounted infinite rule refuses). Expansion has a budget of 2^28 day/position steps (RangeError). */
        all(limit?: number): Date[];
        /** Occurrences in the window; inc makes both ends inclusive. Always equal to all() filtered to the window; a rule without COUNT is not expanded from DTSTART but from the period holding `start`, reached by whole INTERVALs of the rule's period counted from DTSTART, and the expansion stops at the period holding `end`. */
        between(start: Date | number, end: Date | number, inc?: boolean): Date[];
        /** The first occurrence strictly after fromDate, or null; found by the same seek as between(). */
        next(fromDate?: Date | number): Date | null;
        /** The last occurrence strictly before fromDate. */
        prev(fromDate?: Date | number): Date | null;
        /** The rule back in RFC 5545 text form. */
        toString(): string;
    }

    /** Natural-language date parsing per locale. */
    class DateParser {
        /** `opts.now` pins the reference time; it is the only valid key. */
        constructor(locale?: string, opts?: { now?: number });
        /** Unix seconds, or null when nothing matches. */
        parse(text: string): number | null;
        readonly locale: string;
        readonly dayFirst: boolean;
    }

    /** Strict ISO "YYYY-MM-DD" parse (parseTime rejects a seconds field of 60; the RFC 3339 parsers fold it into the next minute). */
    function parseDate(text: string): PlainDate;
    /** The PlainDate `n` days after 1970-01-01. */
    function dateFromEpochDay(n: number): PlainDate;
    /** Strict parse of "HH:MM[:SS[.mmm]]". */
    function parseTime(text: string): PlainTime;
    /** Joins a PlainDate and a PlainTime by pure field copy (the inverse of toPlainDate()/toPlainTime()). */
    function toPlainDateTime(date: PlainDate, time: PlainTime): PlainDateTime;
}

/** dyna:uring — batched disk reads over io_uring; the module exists only in Linux builds with CONFIG_IO_URING=y. */
declare module "dyna:uring" {
    /** Whole file as a string via the io_uring bulk reader (Linux only); no size cap. */
    function readFile(path: import("dyna:file").Path): string;
    /** Whole file as a string via the blocking pread(2) reference reader; no size cap. */
    function readFileSync(path: import("dyna:file").Path): string;
    /** Whole file as a fresh Uint8Array via the io_uring bulk reader; same Path contract as readFile. */
    function readFileBytes(path: import("dyna:file").Path): Uint8Array;
    /** Byte count plus a 32-bit FNV-1a checksum of a whole file; NON-cryptographic (use dyna:hash for integrity). */
    function checksum(path: import("dyna:file").Path, useUring?: boolean): { bytes: number; sum: number };
}

/** dyna:sdl — SDL3 windows, 2D rendering, input events and audio playback; present only in a CONFIG_SDL3=y build. Video calls must run on the main thread (never in an os.Worker). */
declare module "dyna:sdl" {
    type Subsystem = "video" | "audio" | "events" | "joystick" | "gamepad";
    type Rect = { x: number; y: number; width: number; height: number };
    /** One polled event; fields beyond type/timestamp depend on the type ("other" covers kinds not mapped here). */
    interface Event {
        type: "quit" | "keydown" | "keyup" | "textinput" | "textediting" | "dropfile" | "mousemotion" | "mousebuttondown" | "mousebuttonup" | "mousewheel" | "windowshown" | "windowhidden" | "windowmoved" | "windowresized" | "windowfocusgained" | "windowfocuslost" | "windowclose" | "windowexposed" | "windowminimized" | "windowmaximized" | "windowrestored" | "windowmouseenter" | "windowmouseleave" | "windowpixelsizechanged" | "windowdisplayscalechanged" | "fingerdown" | "fingerup" | "fingermotion" | "fingercanceled" | "gamepadadded" | "gamepadremoved" | "gamepadbuttondown" | "gamepadbuttonup" | "gamepadaxis" | "user" | "other";
        /** Milliseconds since SDL started. */
        timestamp: number;
        windowId?: number;
        key?: number;
        keyName?: string;
        scancode?: number;
        mod?: number;
        repeat?: boolean;
        text?: string;
        /** textediting: the selected part of the IME composition text (-1 when unset). */
        start?: number;
        length?: number;
        /** dropfile: the dropped file's path. */
        file?: string;
        x?: number;
        y?: number;
        xrel?: number;
        yrel?: number;
        buttons?: number;
        button?: number;
        clicks?: number;
        data1?: number;
        data2?: number;
        code?: number;
        /** finger*: the touch device and the finger on it; x, y are 0..1 of the window, dx, dy the motion in the same units. */
        touchId?: number;
        fingerId?: number;
        dx?: number;
        dy?: number;
        /** finger*: 0..1. */
        pressure?: number;
        /** gamepad*: the gamepad's id (a Gamepad opens it). */
        which?: number;
        /** gamepadaxis: the axis name and its value, -1..1 (triggers 0..1). gamepadbutton*: `button` is the button's name. */
        axis?: string;
        value?: number;
    }
    /** System cursor names, the CSS cursor vocabulary. */
    type Cursor = "default" | "text" | "wait" | "crosshair" | "progress" | "nwse-resize" | "nesw-resize" | "ew-resize" | "ns-resize" | "move" | "not-allowed" | "pointer" | "nw-resize" | "n-resize" | "ne-resize" | "e-resize" | "se-resize" | "s-resize" | "sw-resize" | "w-resize";
    /** Shows the mouse cursor (needs video). */
    function showCursor(): void;
    /** Hides the mouse cursor (needs video). */
    function hideCursor(): void;
    /** Whether the mouse cursor is shown (needs video). */
    function cursorVisible(): boolean;
    /** Switches to a system cursor; quit() frees the cursors it made (needs video). */
    function setCursor(name: Cursor): void;
    /** Ids of the connected gamepads (needs init(["gamepad"])). */
    function gamepads(): number[];
    /** Connects a software gamepad with every standard button and axis, for tests and input injection; returns its id (needs init(["gamepad"])). */
    function attachVirtualGamepad(name?: string): number;
    /** Disconnects a gamepad attachVirtualGamepad() made; any other id is refused. */
    function detachVirtualGamepad(id: number): void;
    /** Battery state; seconds and percent are null when the platform does not say. */
    function powerInfo(): { state: "error" | "unknown" | "battery" | "no-battery" | "charging" | "charged"; seconds: number | null; percent: number | null };
    /** The desktop's light/dark preference. */
    function systemTheme(): "unknown" | "light" | "dark";
    /** The user's languages in order of preference, as "en-US" or "fr". */
    function preferredLocales(): string[];
    /** Logical CPU cores. */
    function cpuCount(): number;
    /** Physical memory in MiB. */
    function systemRAM(): number;
    /** The directory the application runs from (with a trailing separator), or null when SDL cannot tell. */
    function basePath(): string | null;
    /** Starts subsystems (default ["video"]); repeated calls only add what is missing. Video refuses to start off the main thread. */
    function init(subsystems?: Subsystem[]): void;
    /** Shuts down every subsystem init() started; refused while a Window, AudioStream or Gamepad is still open. */
    function quit(): void;
    /** The subsystems currently running. */
    function wasInit(): Subsystem[];
    /** The linked SDL library's version. */
    function version(): { major: number; minor: number; patch: number; revision: string };
    /** "macOS", "Linux", "Windows", ... */
    function platform(): string;
    /** The last SDL error text ("" when none). */
    function getError(): string;
    /** Video drivers compiled into SDL; set SDL_VIDEODRIVER (e.g. "dummy" for headless) before init to pick one. */
    function videoDrivers(): string[];
    /** The running video driver, or null before init(["video"]). */
    function currentVideoDriver(): string | null;
    /** Audio drivers compiled into SDL; SDL_AUDIODRIVER picks one. */
    function audioDrivers(): string[];
    /** The running audio driver, or null before init(["audio"]). */
    function currentAudioDriver(): string | null;
    /** Milliseconds since SDL was first used. */
    function ticks(): number;
    /** Connected displays (needs video). */
    function displays(): { id: number; name: string | null; x: number; y: number; width: number; height: number; refreshRate: number; pixelDensity: number }[];
    /** The next queued event, or null; main thread only. Call it every frame (e.g. from setInterval) to keep windows responsive. */
    function pollEvent(): Event | null;
    /** Drains up to `max` queued events (default 256, at most 65536). */
    function pollEvents(max?: number): Event[];
    /** Queues a synthetic event that pollEvent() returns with the same fields (input injection, tests); unknown keys and fields of another event type are refused by name, only own enumerable fields are read, finger x/y are 0..1, dx/dy -1..1, pressure 0..1, button 1..5. */
    function pushEvent(event: { type: "user" | "quit" | "keydown" | "keyup" | "mousemotion" | "mousebuttondown" | "mousebuttonup" | "mousewheel" | "fingerdown" | "fingerup" | "fingermotion" | "fingercanceled"; code?: number; windowId?: number; key?: number; scancode?: number; mod?: number; repeat?: boolean; x?: number; y?: number; xrel?: number; yrel?: number; buttons?: number; button?: number; clicks?: number; touchId?: number; fingerId?: number; dx?: number; dy?: number; pressure?: number }): void;
    /** The display name of a keycode ("" for an unknown one, including codes that are no character). */
    function keyName(key: number): string;
    /** The keycode for a key name (0 when unknown). */
    function keyFromName(name: string): number;
    /** Pointer position relative to the focused window, and the pressed-button mask (needs video). */
    function mouseState(): { x: number; y: number; buttons: number };
    /** Sets an SDL hint by name (e.g. "SDL_RENDER_DRIVER"); true when SDL accepted it. Most hints must be set before init. */
    function setHint(name: string, value: string): boolean;
    /** Clipboard text ("" when empty; needs video). */
    function getClipboardText(): string;
    /** Whether the clipboard holds text (needs video). */
    function hasClipboardText(): boolean;
    /** Replaces the clipboard text (needs video). */
    function setClipboardText(text: string): void;
    /** Playback devices, or recording devices with { recording: true }: SDL's id and display name for each (needs init(["audio"])). */
    function audioDevices(opts?: { recording?: boolean }): { id: number; name: string }[];
    /** Decodes a WAV file to interleaved f32 samples in -1..1 (at most 256 MiB of decoded audio). */
    function loadWAV(path: string): { freq: number; channels: number; samples: Float32Array };
    /** Loads a BMP or PNG file into an RGBA Texture of the renderer (each side 1..16384). */
    function loadTexture(renderer: Renderer, path: string): Texture;
    /** A window; width and height are 1..16384. Closing it also closes its Renderer and that renderer's Textures. */
    class Window {
        constructor(title: string, width: number, height: number, opts?: { resizable?: boolean; hidden?: boolean; borderless?: boolean; highDensity?: boolean; fullscreen?: boolean; alwaysOnTop?: boolean });
        readonly id: number;
        readonly title: string;
        /** Size in window coordinates. */
        readonly size: { width: number; height: number };
        /** Size in pixels (larger than size on a high-density display). */
        readonly pixelSize: { width: number; height: number };
        readonly position: { x: number; y: number };
        setTitle(title: string): void;
        setSize(width: number, height: number): void;
        setPosition(x: number, y: number): void;
        show(): void;
        hide(): void;
        raise(): void;
        setFullscreen(on: boolean): void;
        /** Content scale of the window's display (2 on a typical high-density screen). */
        readonly displayScale: number;
        /** Whether textinput and textediting events are being delivered to this window. */
        readonly textInputActive: boolean;
        /** Smallest size the user can resize to; 0 removes the limit. */
        setMinimumSize(width: number, height: number): void;
        /** Largest size the user can resize to; 0 removes the limit. */
        setMaximumSize(width: number, height: number): void;
        /** Opacity 0..1; a video driver without window opacity throws. */
        setOpacity(opacity: number): void;
        /** Starts delivering textinput (and IME textediting) events for this window; a text field needs it. */
        startTextInput(): void;
        /** Stops textinput and textediting events for this window. */
        stopTextInput(): void;
        /** Where the text being edited sits (cursor is the caret's x offset inside rect), so an IME can place its candidate window; null clears it. */
        setTextInputArea(rect: Rect | null, cursor?: number): void;
        /** The window's flags as SDL reports them now (a window manager may apply a request later; sync() waits for it). */
        readonly state: { fullscreen: boolean; hidden: boolean; borderless: boolean; resizable: boolean; minimized: boolean; maximized: boolean; mouseGrabbed: boolean; inputFocus: boolean; mouseFocus: boolean; highDensity: boolean; relativeMouse: boolean; alwaysOnTop: boolean; occluded: boolean };
        maximize(): void;
        minimize(): void;
        restore(): void;
        /** Asks the desktop to draw attention to the window briefly. */
        flash(): void;
        /** Waits until pending size, position and state requests have been applied; throws when the window manager does not apply them in time. */
        sync(): void;
        setResizable(on: boolean): void;
        setBordered(on: boolean): void;
        setAlwaysOnTop(on: boolean): void;
        /** Confines the mouse to the window. */
        setMouseGrab(on: boolean): void;
        /** Hides the cursor and reports only relative motion (xrel/yrel), for mouse-look. */
        setRelativeMouseMode(on: boolean): void;
        /** Captures system shortcuts (Alt-Tab and the like) while the window has focus, where the platform allows it. */
        setKeyboardGrab(on: boolean): void;
        close(): void;
        dispose(): void;
        readonly closed: boolean;
        readonly [Symbol.dispose]: () => void;
    }
    /** A 2D renderer for one window (SDL allows one per window); it keeps its Window alive. */
    class Renderer {
        constructor(window: Window, opts?: { driver?: string; vsync?: boolean });
        /** The backend in use ("metal", "opengl", "software", ...). */
        readonly name: string;
        readonly outputSize: { width: number; height: number };
        /** Channels are integers 0..255; alpha defaults to 255. */
        setDrawColor(r: number, g: number, b: number, a?: number): void;
        clear(): void;
        fillRect(x: number, y: number, width: number, height: number): void;
        drawRect(x: number, y: number, width: number, height: number): void;
        drawLine(x1: number, y1: number, x2: number, y2: number): void;
        drawPoint(x: number, y: number): void;
        /** Draws a Texture of this renderer; src/dst default to the whole texture / whole target. With opts, it turns `angle` degrees clockwise about `center` (dst coordinates, default the middle of dst) and mirrors by `flip`. */
        drawTexture(texture: Texture, src?: Rect | null, dst?: Rect | null, opts?: { angle?: number; center?: { x: number; y: number }; flip?: "none" | "horizontal" | "vertical" | "both" }): void;
        /** Reads back what has been drawn as RGBA rows (call it before present(), which may discard the frame). */
        readPixels(rect?: Rect): { width: number; height: number; pixels: Uint8Array };
        /** Draws into a { target: true } Texture of this renderer until setTarget(null) returns to the window. */
        setTarget(texture: Texture | null): void;
        /** Limits drawing to rect in viewport coordinates; null removes the clip. */
        setClipRect(rect: Rect | null): void;
        /** Maps drawing to rect of the current target; null restores the whole target. */
        setViewport(rect: Rect | null): void;
        /** Scales drawing coordinates; each factor is in (0, 1024]. */
        setScale(scaleX: number, scaleY: number): void;
        /** Blending for fill/draw calls (a Texture keeps its own mode). */
        setBlendMode(mode: "none" | "blend" | "add" | "mod" | "mul"): void;
        /** Turns vsync on or off; a driver without vsync throws. */
        setVSync(on: boolean): void;
        /** Triangles from 8 floats per vertex (x, y, then r, g, b, a and u, v in 0..1); indices, or else the vertices taken in threes, form them. */
        geometry(vertices: Float32Array, indices?: Int32Array | null, texture?: Texture | null): void;
        /** Draws text in SDL's built-in 8x8 font in the draw colour (characters outside ASCII show as '?'). */
        debugText(x: number, y: number, text: string): void;
        /** Draws at a fixed width x height and fits that to the output (mode default "letterbox"); "disabled" with 0, 0 turns it off. Events keep window coordinates: map them with windowToRender. */
        setLogicalPresentation(width: number, height: number, mode?: "disabled" | "stretch" | "letterbox" | "overscan" | "integer"): void;
        /** A window point in render coordinates (after logical presentation, viewport and scale). */
        windowToRender(x: number, y: number): { x: number; y: number };
        /** A render point in window coordinates. */
        renderToWindow(x: number, y: number): { x: number; y: number };
        /** Shows the frame. */
        present(): void;
        close(): void;
        dispose(): void;
        readonly closed: boolean;
        readonly [Symbol.dispose]: () => void;
    }
    /** An RGBA texture owned by a Renderer (blend mode "blend" by default); it keeps its Renderer alive. */
    class Texture {
        /** { target: true } makes a texture Renderer.setTarget can draw into. */
        constructor(renderer: Renderer, width: number, height: number, opts?: { target?: boolean });
        readonly width: number;
        readonly height: number;
        /** True for a texture created with { target: true }. */
        readonly target: boolean;
        /** Uploads RGBA bytes (width*height*4 of the rect, or of the whole texture); the rect must lie inside the texture. */
        update(pixels: ByteView, rect?: Rect): void;
        setAlphaMod(alpha: number): void;
        setColorMod(r: number, g: number, b: number): void;
        setBlendMode(mode: "none" | "blend" | "add" | "mod" | "mul"): void;
        close(): void;
        dispose(): void;
        readonly closed: boolean;
        readonly [Symbol.dispose]: () => void;
    }
    /** Playback, or recording with { recording: true }, on the default device or an audioDevices() id of that kind (needs init(["audio"])); starts paused. format "f32" uses Float32Array samples, "s16" Int16Array; channels interleave. */
    class AudioStream {
        constructor(opts?: { freq?: number; channels?: number; format?: "f32" | "s16"; recording?: boolean; device?: number });
        /** Bytes queued and not yet played. */
        readonly queued: number;
        /** Bytes ready to read: captured audio on a recording stream. */
        readonly available: number;
        /** True for a recording stream. */
        readonly recording: boolean;
        /** Queues interleaved samples of the stream's format (playback streams only). */
        put(samples: Float32Array | Int16Array): void;
        /** Reads up to maxSamples captured samples, whole frames only (recording streams only; default and maximum 2^24). */
        get(maxSamples?: number): Float32Array | Int16Array;
        resume(): void;
        pause(): void;
        /** Drops everything queued. */
        clear(): void;
        /** Marks the end of the queued data so it plays out fully. */
        flush(): void;
        /** Volume multiplier 0..16 (1 is unchanged). */
        setGain(gain: number): void;
        close(): void;
        dispose(): void;
        readonly closed: boolean;
        readonly [Symbol.dispose]: () => void;
    }
    /** An opened gamepad (needs init(["gamepad"])); button and axis names are SDL's ("a", "b", "x", "y", "back", "guide", "start", "leftstick", "rightstick", "leftshoulder", "rightshoulder", "dpup", "dpdown", "dpleft", "dpright"; "leftx", "lefty", "rightx", "righty", "lefttrigger", "righttrigger"). */
    class Gamepad {
        constructor(id: number);
        readonly id: number;
        readonly name: string;
        /** SDL's controller family, e.g. "xbox360", "ps5", "standard". */
        readonly type: string;
        readonly connected: boolean;
        /** True for a gamepad from attachVirtualGamepad(). */
        readonly virtual: boolean;
        button(name: string): boolean;
        /** -1..1; triggers read 0..1. */
        axis(name: string): number;
        /** Vibrates the two motors (intensity 0..1) for ms milliseconds (at most 60000); false when the device cannot. */
        rumble(low: number, high: number, ms: number): boolean;
        /** Presses or releases a button of a virtual gamepad; SDL drops presses while a window exists and none has focus unless the hint SDL_JOYSTICK_ALLOW_BACKGROUND_EVENTS is "1". */
        setVirtualButton(name: string, down: boolean): void;
        /** Moves an axis of a virtual gamepad, value -1..1. */
        setVirtualAxis(name: string, value: number): void;
        close(): void;
        dispose(): void;
        readonly closed: boolean;
        readonly [Symbol.dispose]: () => void;
    }
}

/** dyna:raylib — raylib 6 (opt-in: CONFIG_RAYLIB=y, ./install.sh --with-raylib): one window per process, main thread only. Colours are 0xRRGGBBAA integers. */
declare module "dyna:raylib" {
    /** A colour as 0xRRGGBBAA, e.g. 0xff0000ff for opaque red (see rgba() and colors). */
    type Color = number;
    type Rect = { x: number; y: number; width: number; height: number };
    type Vec2 = { x: number; y: number };
    type Vec3 = { x: number; y: number; z: number };
    type Camera2D = { offset?: Vec2; target?: Vec2; rotation?: number; zoom?: number };
    type Camera3D = { position?: Vec3; target?: Vec3; up?: Vec3; fovy?: number; projection?: "perspective" | "orthographic" };
    /** raylib's named colours (RAYWHITE, RED, BLANK, ...) as 0xRRGGBBAA. */
    const colors: Readonly<Record<string, Color>>;
    /** Keyboard codes by raylib name without the KEY_ prefix (A, SPACE, ESCAPE, F1, KP_0, ...). */
    const keys: Readonly<Record<string, number>>;
    /** LEFT, RIGHT, MIDDLE, SIDE, EXTRA, FORWARD, BACK. */
    const mouseButtons: Readonly<Record<string, number>>;
    /** Gamepad buttons by raylib name without the GAMEPAD_BUTTON_ prefix. */
    const gamepadButtons: Readonly<Record<string, number>>;
    /** LEFT_X, LEFT_Y, RIGHT_X, RIGHT_Y, LEFT_TRIGGER, RIGHT_TRIGGER. */
    const gamepadAxes: Readonly<Record<string, number>>;
    /** The raylib version this binary was compiled against ("6.2"). */
    function version(): string;
    /** raylib's own log threshold (default "warning"). */
    function setLogLevel(level: "all" | "trace" | "debug" | "info" | "warning" | "error" | "fatal" | "none"): void;
    /** Packs channels 0..255 into a Color; alpha defaults to 255. */
    function rgba(r: number, g: number, b: number, a?: number): Color;
    /** Opens the default audio device; idempotent, throws when none exists. */
    function initAudio(): void;
    /** Closes the audio device and unloads every Sound, Music and AudioStream. */
    function closeAudio(): void;
    function audioReady(): boolean;
    /** Volume 0..1 for everything played. */
    function setMasterVolume(volume: number): void;
    /** Fills the current target (window frame or RenderTexture); every draw call needs an open Window. */
    function clearBackground(color: Color): void;
    function drawPixel(x: number, y: number, color: Color): void;
    /** thick defaults to 1 (0..4096). */
    function drawLine(x1: number, y1: number, x2: number, y2: number, color: Color, thick?: number): void;
    function drawCircle(cx: number, cy: number, radius: number, color: Color): void;
    function drawCircleLines(cx: number, cy: number, radius: number, color: Color): void;
    function drawEllipse(cx: number, cy: number, rx: number, ry: number, color: Color): void;
    function drawRectangle(x: number, y: number, width: number, height: number, color: Color): void;
    /** thick defaults to 1 (0..4096). */
    function drawRectangleLines(x: number, y: number, width: number, height: number, color: Color, thick?: number): void;
    /** roundness 0..1, segments 0..1024 (0 picks a count from the radius). */
    function drawRectangleRounded(x: number, y: number, width: number, height: number, roundness: number, segments: number, color: Color): void;
    /** Vertices in counter-clockwise order; a clockwise triangle draws nothing (raylib culls it). */
    function drawTriangle(x1: number, y1: number, x2: number, y2: number, x3: number, y3: number, color: Color): void;
    /** A regular polygon of 3..4096 sides. */
    function drawPoly(cx: number, cy: number, sides: number, radius: number, rotation: number, color: Color): void;
    /** Draws inside mode3D(). */
    function drawCube(x: number, y: number, z: number, width: number, height: number, length: number, color: Color): void;
    function drawCubeWires(x: number, y: number, z: number, width: number, height: number, length: number, color: Color): void;
    function drawSphere(x: number, y: number, z: number, radius: number, color: Color): void;
    /** An XZ plane centred at (x, y, z). */
    function drawPlane(x: number, y: number, z: number, width: number, length: number, color: Color): void;
    function drawLine3D(x1: number, y1: number, z1: number, x2: number, y2: number, z2: number, color: Color): void;
    /** slices 1..1024 around the origin on the XZ plane. */
    function drawGrid(slices: number, spacing: number): void;
    function drawFPS(x: number, y: number): void;
    /** The default font; fontSize is an integer 1..4096. */
    function drawText(text: string, x: number, y: number, fontSize: number, color: Color): void;
    /** Width in pixels of text in the default font. */
    function measureText(text: string, fontSize: number): number;
    /** font null uses the default font; spacing is extra pixels between glyphs. */
    function drawTextEx(font: Font | null, text: string, x: number, y: number, fontSize: number, spacing: number, color: Color): void;
    function measureTextEx(font: Font | null, text: string, fontSize: number, spacing: number): { width: number; height: number };
    /** Draws a Texture or RenderTexture at its own size (a RenderTexture upright); tint defaults to white. */
    function drawTexture(texture: Texture | RenderTexture, x: number, y: number, tint?: Color): void;
    /** src (texture pixels, top-left origin even for a RenderTexture) scaled into dst, rotated in degrees about dst.xy + origin. */
    function drawTexturePro(texture: Texture | RenderTexture, src: Rect, dst: Rect, originX?: number, originY?: number, rotation?: number, tint?: Color): void;
    /** Any image raylib decodes (PNG, BMP, TGA, JPG, GIF, QOI, HDR ...) as an RGBA texture; each side up to 16384, 2^26 pixels in all. */
    function loadTexture(path: string): Texture;
    /** The same formats decoded into a CPU-side RGBA Image (no Window needed). */
    function loadImage(path: string): Image;
    /** A TTF/OTF font rasterised at size 1..1024 (default 32) for ASCII. */
    function loadFont(path: string, size?: number): Font;
    /** WAV, OGG, MP3, FLAC or QOA decoded into memory (needs initAudio()). */
    function loadSound(path: string): Sound;
    /** The same formats streamed from the file; call update() every frame while it plays. */
    function loadMusic(path: string): Music;
    /** Decodes audio to interleaved f32 samples in -1..1 without an audio device (at most 2^28 samples). */
    function loadWave(path: string): { sampleRate: number; channels: number; samples: Float32Array };
    /** Keyboard state from the last frame's event poll; key is 0..511 (see keys). */
    function isKeyDown(key: number): boolean;
    function isKeyPressed(key: number): boolean;
    function isKeyReleased(key: number): boolean;
    /** The next queued key press, 0 when none. */
    function getKeyPressed(): number;
    /** The next queued typed character, "" when none. */
    function getCharPressed(): string;
    function mousePosition(): Vec2;
    function mouseDelta(): Vec2;
    /** Wheel movement this frame (positive is away from the user). */
    function mouseWheel(): number;
    /** button is 0..6 (see mouseButtons). */
    function isMouseButtonDown(button: number): boolean;
    function isMouseButtonPressed(button: number): boolean;
    function isMouseButtonReleased(button: number): boolean;
    /** gamepad is 0..3. */
    function isGamepadAvailable(gamepad: number): boolean;
    /** null when no pad is connected there. */
    function gamepadName(gamepad: number): string | null;
    /** axis 0..5 (see gamepadAxes); -1..1. */
    function gamepadAxis(gamepad: number, axis: number): number;
    /** button 0..17 (see gamepadButtons). */
    function isGamepadButtonDown(gamepad: number, button: number): boolean;
    /** True in the frame the button went down. */
    function isGamepadButtonPressed(gamepad: number, button: number): boolean;
    /** True in the frame the button came up. */
    function isGamepadButtonReleased(gamepad: number, button: number): boolean;
    /** Axes the pad reports, 0 when none is connected there. */
    function gamepadAxisCount(gamepad: number): number;
    /** The last gamepad button pressed on any pad (see gamepadButtons), 0 when none. */
    function getGamepadButtonPressed(): number;
    /** Motor intensities 0..1 for seconds 0..60; a pad without rumble ignores it. */
    function setGamepadVibration(gamepad: number, leftMotor: number, rightMotor: number, seconds: number): void;
    /** Wheel movement this frame on both axes (x is horizontal scrolling). */
    function mouseWheelV(): Vec2;
    /** An annulus between the two radii from startAngle to endAngle degrees; segments 0..4096 (0 picks a count). */
    function drawRing(cx: number, cy: number, innerRadius: number, outerRadius: number, startAngle: number, endAngle: number, segments: number, color: Color): void;
    /** A filled pie slice from startAngle to endAngle degrees; segments 0..4096 (0 picks a count). */
    function drawCircleSector(cx: number, cy: number, radius: number, startAngle: number, endAngle: number, segments: number, color: Color): void;
    /** A rectangle whose colour blends between its four corners. */
    function drawRectangleGradient(x: number, y: number, width: number, height: number, topLeft: Color, bottomLeft: Color, topRight: Color, bottomRight: Color): void;
    /** Connected segments through 2..65536 points (x, y pairs); thick above 0 draws them that wide. */
    function drawLineStrip(points: Float32Array, color: Color, thick?: number): void;
    /** A convex polygon fanned from the first of 3..65536 points, given counter-clockwise. */
    function drawTriangleFan(points: Float32Array, color: Color): void;
    /** A screen point in the world of a mode2D() camera (no Window needed). */
    function screenToWorld2D(x: number, y: number, camera: Camera2D): Vec2;
    /** A world point on the screen of a mode2D() camera (no Window needed). */
    function worldToScreen2D(x: number, y: number, camera: Camera2D): Vec2;
    /** A world point on the window of a mode3D() camera. */
    function worldToScreen(x: number, y: number, z: number, camera: Camera3D): Vec2;
    function showCursor(): void;
    function hideCursor(): void;
    /** Shows the cursor and frees it from disableCursor(). */
    function enableCursor(): void;
    /** Hides the cursor and locks it to the window, for mouse-look (mouseDelta keeps reporting motion). */
    function disableCursor(): void;
    function isCursorHidden(): boolean;
    function isCursorOnScreen(): boolean;
    function setMouseCursor(cursor: "default" | "arrow" | "ibeam" | "crosshair" | "pointer" | "resize-ew" | "resize-ns" | "resize-nwse" | "resize-nesw" | "resize-all" | "not-allowed"): void;
    /** Clipboard text ("" when empty). */
    function getClipboardText(): string;
    function setClipboardText(text: string): void;
    /** The connected monitors; sizes are the current video mode, physical sizes in millimetres. */
    function monitors(): { name: string; x: number; y: number; width: number; height: number; physicalWidth: number; physicalHeight: number; refreshRate: number }[];
    /** Touch points down this frame (0 on a desktop without a touchscreen). */
    function touchCount(): number;
    /** Touch point index 0..7 in window coordinates, null past touchCount(). */
    function touchPoint(index: number): { x: number; y: number; id: number } | null;
    /** The gesture raylib recognised from touch or the left mouse button this frame. */
    function gesture(): "none" | "tap" | "doubletap" | "hold" | "drag" | "swipe-right" | "swipe-left" | "swipe-up" | "swipe-down" | "pinch-in" | "pinch-out";
    /** Measurements of the gesture in progress: hold seconds, drag and pinch vectors, angles in degrees. */
    function gestureDetails(): { holdDuration: number; drag: Vec2; dragAngle: number; pinch: Vec2; pinchAngle: number };
    /** The process's one raylib window (1..16384 a side); closing it unloads every Texture, RenderTexture and Font. fps 0 means uncapped. */
    class Window {
        constructor(width: number, height: number, title: string, opts?: { hidden?: boolean; resizable?: boolean; highDensity?: boolean; vsync?: boolean; msaa?: boolean; undecorated?: boolean; topmost?: boolean; transparent?: boolean; fps?: number });
        /** True once the user closed the window or pressed the exit key (ESC by default). */
        readonly shouldClose: boolean;
        readonly size: { width: number; height: number };
        /** Framebuffer size in pixels (larger than size on a high-density display with highDensity). */
        readonly renderSize: { width: number; height: number };
        readonly scaleDPI: Vec2;
        readonly focused: boolean;
        readonly minimized: boolean;
        readonly fps: number;
        /** Seconds the last frame took. */
        readonly frameTime: number;
        /** Seconds since the window opened. */
        readonly time: number;
        setTitle(title: string): void;
        /** size then reports width x height. On macOS windows are always high-density (highDensity is implied), so size stays in points and renderSize in pixels across resizes. */
        setSize(width: number, height: number): void;
        setPosition(x: number, y: number): void;
        /** 0..1000; frame() then waits out the rest of each frame's slot. */
        setTargetFPS(fps: number): void;
        /** The key that sets shouldClose (0 disables it). */
        setExitKey(key: number): void;
        /** Runs draw between BeginDrawing and EndDrawing (which presents and polls input); draw must be synchronous and frame() cannot nest. Closing the window inside it takes effect when it returns. */
        frame(draw: () => void): void;
        /** Runs draw with a 2D camera, inside frame() or RenderTexture#draw(); zoom must be above 0. */
        mode2D(camera: Camera2D, draw: () => void): void;
        /** Runs draw with a 3D camera (fovy in (0, 180), default 45, perspective). */
        mode3D(camera: Camera3D, draw: () => void): void;
        /** Runs draw with a blend mode, inside frame() or RenderTexture#draw(); it does not nest in itself. */
        blendMode(mode: "alpha" | "additive" | "multiplied" | "add-colors" | "subtract-colors" | "alpha-premultiply", draw: () => void): void;
        /** Runs draw clipped to a rectangle of the target in pixels (not camera coordinates), inside frame() or RenderTexture#draw(); it does not nest in itself. */
        scissor(x: number, y: number, width: number, height: number, draw: () => void): void;
        readonly fullscreen: boolean;
        readonly maximized: boolean;
        readonly hidden: boolean;
        /** True in the frame after the window changed size. */
        readonly resized: boolean;
        readonly position: Vec2;
        /** Index into monitors() of the monitor the window is on. */
        readonly monitor: number;
        /** The state changes below run outside frame() and RenderTexture#draw(). */
        toggleFullscreen(): void;
        toggleBorderless(): void;
        maximize(): void;
        minimize(): void;
        restore(): void;
        focus(): void;
        setResizable(on: boolean): void;
        setTopmost(on: boolean): void;
        /** false removes the title bar and border. */
        setDecorated(on: boolean): void;
        /** Keeps frames running while the window is minimized or unfocused. */
        setAlwaysRun(on: boolean): void;
        /** Lets clicks fall through to what is behind the window. */
        setMousePassthrough(on: boolean): void;
        /** 0..1. */
        setOpacity(opacity: number): void;
        /** Limits user resizing; 1..16384 a side. */
        setMinSize(width: number, height: number): void;
        setMaxSize(width: number, height: number): void;
        close(): void;
        dispose(): void;
        readonly closed: boolean;
        readonly [Symbol.dispose]: () => void;
    }
    /** An RGBA GPU texture from loadTexture() or Image#toTexture(); it keeps its Window alive. */
    class Texture {
        readonly width: number;
        readonly height: number;
        /** Uploads RGBA bytes for the rect (whole pixels inside the texture) or the whole texture. */
        update(pixels: Uint8Array, rect?: Rect): void;
        setFilter(mode: "point" | "bilinear" | "trilinear"): void;
        close(): void;
        dispose(): void;
        readonly closed: boolean;
        readonly [Symbol.dispose]: () => void;
    }
    /** An offscreen RGBA target (each side up to 16384, 2^26 pixels); it keeps its Window alive. */
    class RenderTexture {
        constructor(width: number, height: number);
        readonly width: number;
        readonly height: number;
        /** Runs draw with this texture as the target; it cannot nest inside another draw(), mode2D() or mode3D(). */
        draw(draw: () => void): void;
        /** The texture's pixels as top-to-bottom RGBA rows. */
        readPixels(): { width: number; height: number; pixels: Uint8Array };
        setFilter(mode: "point" | "bilinear" | "trilinear"): void;
        close(): void;
        dispose(): void;
        readonly closed: boolean;
        readonly [Symbol.dispose]: () => void;
    }
    /** A CPU-side RGBA image (no Window needed until toTexture()); each side up to 16384, 2^26 pixels in all. */
    class Image {
        /** fill is a Color (default transparent) or exactly width*height*4 RGBA bytes. */
        constructor(width: number, height: number, fill?: Color | Uint8Array);
        readonly width: number;
        readonly height: number;
        /** A copy of the RGBA bytes. */
        pixels(): Uint8Array;
        /** The extension picks the format: .png, .bmp, .qoi or .tga. */
        export(path: string): void;
        resize(width: number, height: number): void;
        /** The rect must lie inside the image. */
        crop(x: number, y: number, width: number, height: number): void;
        flipVertical(): void;
        flipHorizontal(): void;
        copy(): Image;
        toTexture(): Texture;
        /** CPU drawing straight into the pixels (no Window needed, except drawText): whole-pixel coordinates within +-65536, clipped to the image. */
        clear(color: Color): void;
        drawPixel(x: number, y: number, color: Color): void;
        /** thick 1..4096 (default 1). */
        drawLine(x1: number, y1: number, x2: number, y2: number, color: Color, thick?: number): void;
        drawRectangle(x: number, y: number, width: number, height: number, color: Color): void;
        /** An outline thick pixels wide (1..4096, default 1) inside the rectangle. */
        drawRectangleLines(x: number, y: number, width: number, height: number, color: Color, thick?: number): void;
        drawCircle(cx: number, cy: number, radius: number, color: Color): void;
        drawCircleLines(cx: number, cy: number, radius: number, color: Color): void;
        drawTriangle(x1: number, y1: number, x2: number, y2: number, x3: number, y3: number, color: Color): void;
        /** Alpha-blends another Image (or this one) at x, y, multiplied by tint (default white). */
        drawImage(source: Image, x: number, y: number, tint?: Color): void;
        /** Text in the default font, which needs an open Window. */
        drawText(text: string, x: number, y: number, fontSize: number, color: Color): void;
        close(): void;
        dispose(): void;
        readonly closed: boolean;
        readonly [Symbol.dispose]: () => void;
    }
    /** A rasterised font from loadFont(); it keeps its Window alive. */
    class Font {
        readonly baseSize: number;
        readonly glyphCount: number;
        close(): void;
        dispose(): void;
        readonly closed: boolean;
        readonly [Symbol.dispose]: () => void;
    }
    /** A decoded sound from loadSound(); closeAudio() unloads it. */
    class Sound {
        readonly playing: boolean;
        play(): void;
        stop(): void;
        pause(): void;
        resume(): void;
        /** 0..1. */
        setVolume(volume: number): void;
        /** 0.01..16, 1 is unchanged. */
        setPitch(pitch: number): void;
        /** 0 left .. 1 right. */
        setPan(pan: number): void;
        close(): void;
        dispose(): void;
        readonly closed: boolean;
        readonly [Symbol.dispose]: () => void;
    }
    /** Streamed audio from loadMusic(); update() it every frame while it plays. */
    class Music {
        readonly playing: boolean;
        /** Length in seconds. */
        readonly length: number;
        /** Seconds played so far; seek() sets it, and update() then subtracts what it buffers ahead. */
        readonly played: number;
        play(): void;
        stop(): void;
        pause(): void;
        resume(): void;
        /** Refills the stream's buffers. */
        update(): void;
        setVolume(volume: number): void;
        setPitch(pitch: number): void;
        setPan(pan: number): void;
        /** seconds must not pass length. */
        seek(seconds: number): void;
        setLooping(on: boolean): void;
        close(): void;
        dispose(): void;
        readonly closed: boolean;
        readonly [Symbol.dispose]: () => void;
    }
    /** Raw f32 output, 8000..192000 Hz, 1 or 2 channels (needs initAudio()). */
    class AudioStream {
        constructor(sampleRate: number, channels: number);
        readonly playing: boolean;
        /** True when the stream wants its next buffer. */
        readonly processed: boolean;
        play(): void;
        stop(): void;
        pause(): void;
        resume(): void;
        /** Queues whole frames of interleaved samples; refused until processed is true. */
        update(samples: Float32Array): void;
        setVolume(volume: number): void;
        setPitch(pitch: number): void;
        setPan(pan: number): void;
        close(): void;
        dispose(): void;
        readonly closed: boolean;
        readonly [Symbol.dispose]: () => void;
    }
}

/** dyna:url — WHATWG URL and URLSearchParams (the same classes as the globals), IDNA 2008 / UTS #46, Punycode, form encoding. */
declare module "dyna:ui" {
    /** A colour as 0xRRGGBBAA. */
    type Color = number;
    /** A length in logical points: a number of points, "fit" (to content), "grow" (share free space), "N%" (of the parent's inner size) or { grow: weight } (weight 0 does not grow). */
    type Size = number | "fit" | "grow" | `${number}%` | { grow: number };
    type Rect = { x: number; y: number; width: number; height: number };
    /** Layout and style of a container; every length is in logical points and is multiplied by the Context's scale. */
    type BoxOptions = {
        width?: Size; height?: Size; minWidth?: number; maxWidth?: number; minHeight?: number; maxHeight?: number;
        /** A number, [x, y] or [left, top, right, bottom]. */
        padding?: number | [number, number] | [number, number, number, number];
        /** Space between children; rows default to the theme's padX, columns to its spacing. */
        gap?: number;
        alignX?: "start" | "center" | "end"; alignY?: "start" | "center" | "end";
        /** Clips the children and scrolls them (wheel, drag on touch, scrollbar); needs an id. */
        scroll?: boolean | "x" | "y" | "both";
        /** A row whose children flow onto further lines when they do not fit (flex-wrap). */
        wrap?: boolean;
        fill?: Color; border?: Color; borderWidth?: number; radius?: number;
        /** Keys the container's retained state (scroll offset) and scopes the ids of what it holds. */
        id?: string;
        /** Takes the box out of flow: "parent" at (x, y) inside its parent; "popup" relative to its parent, above everything, kept inside the safe area; "screen" at (x, y) on the screen, free to cover the insets (edge-to-edge bars, shades); "safe" at (x, y) on the screen, moved inside the safe area. */
        float?: "parent" | "popup" | "screen" | "safe"; x?: number; y?: number;
        /** Stacking order among floating boxes; equal values keep declaration order. */
        z?: number;
    };
    /** Theme values; lengths are logical points, colours 0xRRGGBBAA. */
    type Theme = {
        /** Font index: -1 picks ProggyClean for 13 px text and Inter for every other size; 0 is ProggyClean, 1 Inter, then loadFont() results. */
        font: number; fontSize: number; padX: number; padY: number; spacing: number; windowPad: number; indent: number; radius: number;
        borderWidth: number; labelWidth: number; scrollbarWidth: number;
        /** The smallest height of anything touchable (44 suits fingers, 0 a mouse). */
        minTarget: number;
        /** How far a touch moves before it becomes a scroll instead of a tap. */
        dragSlop: number;
        /** Seconds a touch holds still before it raises the context action. */
        longPress: number;
        /** Multiplies every text size (an accessibility text scale). */
        textScale: number;
        text: Color; textDim: Color; onAccent: Color; bg: Color; panel: Color; frame: Color; frameHot: Color; frameActive: Color; button: Color;
        buttonHot: Color; buttonActive: Color; accent: Color; accentHot: Color; border: Color; title: Color; titleActive: Color; selection: Color;
        popup: Color; shade: Color; tooltip: Color; link: Color; scrollbar: Color; scrollbarHot: Color;
        /** The border and message colour of a field given { error }. */
        error: Color;
    };
    /** Colour sets for theme(): dark (the default) and light. */
    const themes: { readonly dark: Readonly<Partial<Theme>>; readonly light: Readonly<Partial<Theme>> };
    /** Metric sets for theme(): compact (mouse, the default) and touch (44 pt targets, roomier padding). */
    const densities: { readonly compact: Readonly<Partial<Theme>>; readonly touch: Readonly<Partial<Theme>> };
    type Geometry = {
        /** 8 floats per vertex in device pixels: x, y, r, g, b, a (0..1), u, v. */
        vertices: Float32Array;
        indices: Uint32Array;
        /** Draw ranges of indices: texture is "atlas" (glyphs and solid shapes, alpha only) or an image handle; clip is in device pixels. */
        batches: { texture: "atlas" | number; first: number; count: number; clip: Rect }[];
    };
    /** An immediate-mode UI: frame(fn) runs fn, lays out what it declared, and draws it to the target only when the result changed. */
    class Context {
        /** Without a target the Context draws in memory (pixels() reads it); with a dyna:sdl Renderer or dyna:raylib Window it draws there and takes size, scale and safe-area insets from it; present: false draws inside the caller's frame (sdl: between its own draws; raylib: inside rl.frame() or RenderTexture#draw()) without clearing or presenting; touch "auto" applies the touch density on Android and iOS. */
        constructor(opts: { width?: number; height?: number; scale?: number; insets?: [number, number, number, number]; touch?: boolean | "auto"; target?: object; present?: boolean; clock?: "monotonic" | "manual"; clear?: Color });
        /** Runs fn(this) to declare the UI, lays it out and draws it; returns whether anything visible changed. */
        frame(fn: (ui: Context) => void): boolean;
        /** Blocks until input arrives or the next timed redraw is due (animations, tooltips, long presses, flings), at most seconds; returns the seconds it waited at most. */
        idle(seconds?: number): number;
        /** RGBA of the last frame in device pixels (pixelWidth x pixelHeight), rasterised in memory for any target. */
        pixels(): Uint8Array;
        /** Rasterises the last frame into target (exactly pixelWidth * pixelHeight * 4 bytes) without allocating, and returns target. */
        pixelsInto(target: Uint8Array): Uint8Array;
        /** The last frame as triangles for a custom renderer. */
        geometry(): Geometry;
        /** The box of the widget or block declared just before (a row/column/box/wrap with an id, group, card, property, panel, scroll, split, popup, modal, drawer, sheet, bar; a tree or menu answers its header), as laid out by the previous frame (inside frame()) or this one (after it), in logical points; null before it has been laid out. */
        lastRect(): Rect | null;
        /** The box of item `item` of the composite declared just before (tabs, segmented, radio, combo items while open, list/log/table rows, navBar/navRail, breadcrumbs, stepper, pagination by page, datePicker by day, rating by star, accordion headers, tagInput chips, timePicker hour/minute/AM-PM, autocomplete suggestions while shown, scaffold destinations); with column, a table cell (item -1 with a column is that column's header); null when that item is not laid out or not visible. */
        lastRect(item: number, column?: number): Rect | null;
        /** Counts for the last frame (elements, text bytes, draw commands, glyph quads, vertices, hit boxes, text runs and their glyph records, cached glyphs, atlas rows) and totals (frames, frames reused because their input was unchanged). */
        stats(): { elements: number; textBytes: number; commands: number; glyphQuads: number; vertices: number; hitBoxes: number; textRuns: number; runRecords: number; glyphs: number; atlasHeight: number; frames: number; reusedFrames: number };
        /** The glyph atlas the "atlas" texture samples: one alpha byte per texel. */
        atlas(): { width: number; height: number; version: number; alpha: Uint8Array };
        /** Resizes a Context without a target (logical size, optional scale and insets). */
        resize(width: number, height: number, opts?: { scale?: number; insets?: [number, number, number, number] }): void;
        /** Reads the theme, or merges the given values into it (between frames). */
        theme(values?: Partial<Theme>): Theme | undefined;
        /** Loads a TRUSTED TrueType/OpenType font from a path or bytes and returns its index for theme({ font }). */
        loadFont(source: string | Uint8Array): number;
        /** Registers RGBA pixels (width * height * 4 bytes) and returns a handle for image(), iconButton(), avatar() and navigation icons. */
        addImage(rgba: Uint8Array, width: number, height: number): number;
        updateImage(handle: number, rgba: Uint8Array): void;
        removeImage(handle: number): void;
        /** Synthetic pointer input in logical points; buttons is 1 left, 2 right, 4 middle; kind "touch" has no hover and scrolls by dragging. */
        pointer(state: { x?: number; y?: number; buttons?: number; wheelX?: number; wheelY?: number; time?: number; kind?: "mouse" | "touch" | "pen" }): void;
        /** Synthetic key input by name: left right up down home end backspace delete return tab escape pageup pagedown a c v x y z space. */
        key(name: string, down?: boolean, mods?: { shift?: boolean; ctrl?: boolean; alt?: boolean; super?: boolean }): void;
        /** Synthetic text input (what a keyboard or IME commits). */
        type(text: string): void;
        /** An IME's uncommitted text, drawn at the caret of the field being edited. */
        composition(text: string, cursor?: number): void;
        /** Sets the clock of a { clock: "manual" } Context, in seconds. */
        setTime(seconds: number): void;
        /** Logical size of the drawable area. */
        readonly width: number;
        readonly height: number;
        readonly pixelWidth: number;
        readonly pixelHeight: number;
        /** Device pixels per logical point. */
        readonly scale: number;
        /** "compact" below 600 points wide, "medium" below 840, else "expanded". */
        readonly sizeClass: "compact" | "medium" | "expanded";
        /** The safe-area insets of the last frame in logical points: [left, top, right, bottom]. */
        readonly insets: [number, number, number, number];
        /** Seconds until a timed redraw is due (Infinity when nothing animates). */
        readonly wakeIn: number;
        /** The caret of the field being edited, in logical points, or null. */
        readonly textInput: Rect | null;
        /** Whether the last text field saw Return this frame. */
        readonly submitted: boolean;
        /** Whether the last text field holds keyboard focus. */
        readonly editing: boolean;
        /** The row the last list or table activated (Return or double click) this frame, or -1. */
        readonly activated: number;
        /** The payload of the drag in progress (this frame), or null. */
        readonly dragging: string | number | null;
        /** A horizontal container; children are centred vertically. */
        row(opts: BoxOptions | ((ui: Context) => void), fn?: (ui: Context) => void): void;
        column(opts: BoxOptions | ((ui: Context) => void), fn?: (ui: Context) => void): void;
        /** A column without default gap or alignment. */
        box(opts: BoxOptions | ((ui: Context) => void), fn?: (ui: Context) => void): void;
        /** A full-width row whose children wrap onto further lines (chips, cards, toolbars). */
        wrap(opts: BoxOptions | ((ui: Context) => void), fn?: (ui: Context) => void): void;
        /** Scopes the ids of the widgets fn declares (for lists of identical labels). */
        id(key: string | number, fn: (ui: Context) => void): void;
        /** Empty space; an omitted length grows. */
        spacer(width?: number, height?: number): void;
        /** Text after "##" in any label is not shown but keys the widget. label, text, heading, bullet, badge, image and progress have a box for lastRect() and tooltip(), keyed by the first 32 bytes and the length of their text: when two in one id scope share both, the first one answers. */
        label(text: string): void;
        text(text: string, opts?: { color?: Color; wrap?: boolean }): void;
        heading(text: string): void;
        bullet(text: string): void;
        separator(title?: string): void;
        link(text: string): boolean;
        badge(text: string, color?: Color): void;
        image(handle: number, width?: number, height?: number, tint?: Color): void;
        progress(fraction: number, width?: number): void;
        /** An animated busy indicator; it asks for the next frame itself (wakeIn). */
        spinner(size?: number): void;
        skeleton(width?: number, height?: number): void;
        avatar(name: string, opts?: { image?: number; size?: number }): void;
        /** True on the frame the button is clicked, or activated by Return/Space while keyboard-focused. */
        button(label: string): boolean;
        iconButton(label: string, image: number, size?: number): boolean;
        /** Each returns the new value; the value passed in is never mutated. */
        toggle(label: string, on: boolean): boolean;
        checkbox(label: string, checked: boolean): boolean;
        switch(label: string, on: boolean): boolean;
        /** Returns the selected state (true every frame while selected), not "clicked". */
        selectable(label: string, selected: boolean): boolean;
        chip(label: string, selected: boolean): boolean;
        radio(label: string, items: string[], selected: number, opts?: { horizontal?: boolean }): number;
        segmented(label: string, items: string[], selected: number): number;
        tabs(label: string, items: string[], selected: number): number;
        /** Up/Down move the highlight and Return picks it while the popup is open; search adds a filter field (case-insensitive substring, cleared each time it opens); error/help draw a line below. */
        combo(label: string, items: string[], selected: number, opts?: { search?: boolean; error?: string; help?: string }): number;
        slider(label: string, value: number, min: number, max: number, opts?: { step?: number; decimals?: number; vertical?: boolean; length?: number }): number;
        /** Returns the same array while unchanged, a new [low, high] when dragged. */
        range(label: string, value: [number, number], min: number, max: number, opts?: { step?: number; decimals?: number }): [number, number];
        /** Dragging sideways changes the value by speed per point moved (1 by default). */
        drag(label: string, value: number, opts?: { speed?: number; min?: number; max?: number; decimals?: number }): number;
        spin(label: string, value: number, opts?: { step?: number; min?: number; max?: number; decimals?: number }): number;
        /** Returns the text after this frame's edits (caret, selection, clipboard, undo); the same string while unchanged. */
        /** Returns the text after this frame's edits (caret, selection, clipboard, undo); the same string while unchanged; error draws the field's border and a message below it in theme error, help a dim line; lastRect() stays the field. */
        textField(label: string, text: string, opts?: { hint?: string; maxLength?: number; width?: number; password?: boolean; readOnly?: boolean; error?: string; help?: string }): string;
        textArea(label: string, text: string, opts?: { hint?: string; maxLength?: number; width?: number; rows?: number; readOnly?: boolean; error?: string; help?: string }): string;
        search(label: string, text: string, opts?: { hint?: string; width?: number; error?: string; help?: string }): string;
        /** A text field with the caller's suggestions below it while focused; a click, or Down/Up and Return, replaces the text with one (caret at the end) and hides them until the text changes; Escape hides them. */
        autocomplete(label: string, text: string, suggestions: string[], opts?: { hint?: string; width?: number; error?: string; help?: string }): string;
        /** Hour and minute fields (Up/Down step them, two typed digits set one) with an AM/PM button when h12; minutes step by step (1 by default; a step must divide 60, others are a RangeError) and wrap on that grid; returns the same object while unchanged. */
        timePicker(label: string, time: { hour: number; minute: number }, opts?: { step?: number; h12?: boolean; error?: string; help?: string }): { hour: number; minute: number };
        /** Chips with a close button and a field: every comma-separated piece of a frame's input is added at once, and Return adds the rest (trimmed; duplicates of a tag or of another piece are skipped), Backspace in the empty field removes the last tag; returns the same array while unchanged. */
        tagInput(label: string, tags: string[], opts?: { hint?: string; error?: string; help?: string }): string[];
        color(label: string, rgba: Color): Color;
        rating(label: string, value: number, opts?: { max?: number }): number;
        /** Returns the same object while unchanged; firstDay 0 is Sunday, 1 (the default) Monday. */
        datePicker(label: string, date: { year: number; month: number; day: number }, opts?: { firstDay?: number }): { year: number; month: number; day: number };
        pagination(label: string, page: number, pages: number): number;
        /** Returns the index of the crumb clicked this frame, or -1 (the last is the current page). */
        breadcrumbs(label: string, items: string[]): number;
        /** Returns the step clicked this frame, or -1. */
        stepper(label: string, items: string[], current: number): number;
        /** A virtualised list: only visible rows read items[]; returns the selected index. */
        list(label: string, items: string[], selected: number, opts?: { rows?: number; follow?: boolean }): number;
        /** A virtualised read-only list that follows its end while scrolled to it. */
        log(label: string, lines: string[], opts?: { rows?: number; follow?: boolean }): void;
        /** A virtualised table: cell(row, column) runs only for visible rows; headers sort, grips resize; Return or a double click activates the selected row. selectedColumn (-1 for none) selects cells: a click picks one, arrows/Home/End move it. edit (with selectedColumn) shows that cell as a focused text field: result edit is its text, committed on Return, on a click elsewhere in the table, or when focus leaves it, cancelled on Escape; on the committing frame selected/column name the edited cell and a clicked cell becomes selected on the next frame. A selected row, selectedColumn or sort column out of range reads as -1; column is -1 without selectedColumn. */
        table(label: string, opts: { columns: { name: string; width?: number }[]; rows: number; cell: (row: number, column: number) => string; visibleRows?: number; selected?: number; selectedColumn?: number; edit?: string; sort?: { column?: number; descending?: boolean } }): { selected: number; column: number; sortColumn: number; descending: boolean; activated: boolean; edit: string | undefined; committed: boolean; cancelled: boolean };
        plot(label: string, values: ArrayLike<number>, opts?: { min?: number; max?: number; height?: number }): void;
        bars(label: string, values: ArrayLike<number>, opts?: { min?: number; max?: number; height?: number }): void;
        /** A label column of theme labelWidth and fn's widgets beside it. */
        property(label: string, fn: (ui: Context) => void): void;
        group(title: string, fn: (ui: Context) => void): void;
        card(title: string, opts: { width?: number } | ((ui: Context) => void), fn?: (ui: Context) => void): void;
        panel(label: string, opts: { width?: number; height?: number; border?: boolean } | ((ui: Context) => void), fn?: (ui: Context) => void): void;
        scroll(label: string, opts: { width?: number; height?: number; border?: boolean } | ((ui: Context) => void), fn?: (ui: Context) => void): void;
        collapsing(label: string, defaultOpen?: boolean): boolean;
        /** Section headers of which at most one is open; fn(ui, index) declares the section that was open when the frame began; returns the open index (-1 for none), changed by a click on a header. */
        accordion(label: string, sections: string[], open: number, fn: (ui: Context, index: number) => void): number;
        /** Returns whether the node is open; fn declares the children (none makes a leaf). */
        tree(label: string, opts?: { open?: boolean; leaf?: boolean } | ((ui: Context) => void), fn?: (ui: Context) => void): boolean;
        /** A floating, draggable window; returns false the frame its close button is clicked. */
        window(title: string, opts: { x?: number; y?: number; width?: number; height?: number; closable?: boolean; collapsible?: boolean; resizable?: boolean; fixed?: boolean } | ((ui: Context) => void), fn?: (ui: Context) => void): boolean;
        /** Two panes split at ratio (0.05..0.95) with a draggable handle; returns the ratio. */
        split(label: string, ratio: number, opts: { vertical?: boolean } | ((ui: Context) => void), first: (ui: Context) => void, second?: (ui: Context) => void): number;
        /** Opens popup(label) at the pointer; both calls must be in the same id scope (a container with an id, or id(), between them names another popup). */
        openPopup(label: string): void;
        /** A popup at the pointer where openPopup() was called; closes on an outside press or Escape. */
        popup(label: string, fn: (ui: Context) => void): boolean;
        /** Opens on a right press or a long touch over the widget declared just before it. */
        contextMenu(label: string, fn: (ui: Context) => void): boolean;
        /** Opens modal(label); modal, drawer and sheet names are screen-wide, so the open call may sit inside any container. */
        openModal(label: string): void;
        modal(label: string, opts: { width?: number } | ((ui: Context) => void), fn?: (ui: Context) => void): boolean;
        /** Closes the innermost popup, modal, drawer or sheet being declared. */
        closePopup(): void;
        /** Shown for the widget declared just before it: on hover after delay seconds, or while a touch holds it. */
        tooltip(text: string, opts?: { delay?: number }): void;
        /** Opens drawer(label); the name is screen-wide, like a modal's. */
        openDrawer(label: string): void;
        drawer(label: string, opts: { side?: "left" | "right"; width?: number } | ((ui: Context) => void), fn?: (ui: Context) => void): boolean;
        /** Opens sheet(label); the name is screen-wide, like a modal's. */
        openSheet(label: string): void;
        /** A bottom sheet over a shade; tapping the shade closes it. */
        sheet(label: string, fn: (ui: Context) => void): boolean;
        /** A transient message at the bottom; callable between frames too. */
        toast(message: string, opts?: { duration?: number }): void;
        /** A floating action button above the bottom bars; a label without an icon makes it extended. */
        fab(label: string, opts?: { icon?: number }): boolean;
        /** Declared directly in frame() these dock to the screen edge (outside the safe area insets) and stack. */
        menubar(fn: (ui: Context) => void): void;
        appBar(title: string, fn: (ui: Context) => void): void;
        navBar(label: string, items: (string | { label: string; icon?: number })[], selected: number): number;
        /** A vertical navigation column that fills its parent's height; it does not dock, so declare it first in a row beside the content. */
        navRail(label: string, items: (string | { label: string; icon?: number })[], selected: number): number;
        /** Adaptive navigation: a navBar docked at the bottom below 600 points wide, else a navRail left of fn's content; returns the selected destination. */
        scaffold(label: string, items: (string | { label: string; icon?: number })[], selected: number, fn: (ui: Context) => void): number;
        /** Makes the widget declared just before it draggable with payload (a TypeError when no widget precedes it in the frame, or the payload is ""); compare dropTarget()'s result with null, since 0 is a payload; true while it is being dragged. A mouse drag starts past dragSlop, a touch one after a long press (or a move no scroll container takes); the release is not a click, Escape cancels; label is drawn beside the pointer. */
        dragSource(payload: string | number, opts?: { label?: string }): boolean;
        /** Makes the widget or block declared just before it a drop target (a TypeError when none precedes it in the frame): highlighted while a drag is over it; returns the payload on the frame a drag is released over it (the smallest target under the pointer wins), else null. */
        dropTarget(): string | number | null;
        /** Declared directly in frame() these dock to the screen edge (outside the safe area insets) and stack. */
        toolbar(fn: (ui: Context) => void): void;
        statusbar(fn: (ui: Context) => void): void;
        menu(label: string, fn: (ui: Context) => void): boolean;
        menuItem(label: string, opts?: { shortcut?: string; checked?: boolean; enabled?: boolean }): boolean;
        /** Frees the context's native state; refused inside frame(). */
        close(): void;
        dispose(): void;
        readonly closed: boolean;
    }
}

declare module "dyna:url" {
    /** WHATWG URL. Every string input is a USVString: a lone surrogate becomes U+FFFD (percent-encoded %EF%BF%BD). Input over 65536 bytes throws; a non-ASCII host over 4096 mapped code points is invalid; href and origin are read-only.
 *  Setters (WHATWG basic URL parser with a state override): every setter but username/password first removes U+0009/000A/000D anywhere in the value; other C0 controls are kept and then percent-encoded (path, query, fragment, userinfo, opaque host) or refused (special host). A NUL is never truncating: it becomes %00, or makes host/hostname a no-op, or ends the port digits. */
    class URL {
        /** Parses `input` against an optional `base` URL; a bad `base` or unparsable `input` throws, and input longer than 65536 bytes throws a RangeError. Non-string input and base go through ToString (WebIDL USVString); a missing input is a TypeError. */
        constructor(input: unknown, base?: unknown);
        /** Parses without throwing: null for every failure the constructor would throw (a missing argument is still a TypeError; other values go through ToString). */
        static parse(input: unknown, base?: unknown): URL | null;
        /** parse() as a predicate. */
        static canParse(input: unknown, base?: unknown): boolean;
        /** Resolves `rel` against `base` and returns the href; THROWS TypeError when either is unparseable. */
        static join(base: string, rel: string): string;
        readonly href: string;
        /** Settable: scheme-start parse of value + ":" (stops at the first ":"; any other non-scheme character is a no-op). Special <-> non-special switches, "file" with credentials or a port, and leaving "file" with an empty host are no-ops; a port equal to the new scheme's default is dropped. */
        protocol: string;
        /** Settable unless the URL has no host, an empty host or the file scheme; percent-encoded with the userinfo set (no tab/newline stripping); "" clears it. */
        username: string;
        /** As username. */
        password: string;
        /** Settable; a `:port` suffix splits off; a forbidden host code point or an empty value makes the assignment a no-op. */
        host: string;
        /** Settable (lowercased; the port is untouched); invalid input is a no-op. */
        hostname: string;
        /** Settable; the leading ASCII digits are kept ("90x" sets 90; no leading digit is a no-op; above 65535 is a no-op); "" clears the port; no-op without a host or for file URLs. */
        port: string;
        /** Settable; parsed in path state: `?` starts the query, `#` the fragment, `.`/`..` resolve. */
        pathname: string;
        /** Settable; one leading `?` is a delimiter; "" clears the query. */
        search: string;
        /** Settable; one leading `#` is a delimiter; "" clears the fragment. */
        hash: string;
        readonly origin: string;
        /** Settable: assigning replaces the whole query with `new URLSearchParams(value)`'s serialization. */
        searchParams: URLSearchParams;
        /** The full href. */
        toJSON(): string;
        /** The full href. */
        toString(): string;
    }
    /** WHATWG query list; names and values are USVStrings (a lone surrogate becomes U+FFFD), and percent-decoded bytes that are not UTF-8 decode to U+FFFD. One obtained from url.searchParams (serialization leaves `~` unescaped, unlike the WHATWG form encoder, which writes %7E; kept deliberately) writes through to that URL. entries/keys/values are live iterators (the *Array() twins return arrays). */
    class URLSearchParams {
        constructor(init?: string | URLSearchParams | Array<[string, string]> | Record<string, string>);
        /** The number of name/value pairs. */
        readonly size: number;
        /** Appends a pair; a name that already exists keeps its other values. */
        append(name: string, value: string): void;
        /** With `value`, deletes only pairs matching BOTH name and value (WHATWG parity). */
        delete(name: string, value?: string): void;
        /** The first value for the name, or `null` when absent. */
        get(name: string): string | null;
        /** Every value for the name. */
        getAll(name: string): string[];
        /** With the optional value only pairs matching BOTH count (WHATWG has(name, value)). */
        has(name: string, value?: string): boolean;
        /** Replaces every pair with that name with a single pair. */
        set(name: string, value: string): void;
        /** Orders pairs by the DECODED key's byte order, in place. */
        sort(): void;
        /** The `name=value&...` form, re-encoded. */
        toString(): string;
        forEach(callback: (value: string, key: string, params: URLSearchParams) => void): void;
        [Symbol.iterator](): IterableIterator<[string, string]>;
        /** Iterator over [name, value] pairs, live between steps. */
        entries(): IterableIterator<[string, string]>;
        /** Iterator over names. */
        keys(): IterableIterator<string>;
        /** Iterator over values. */
        values(): IterableIterator<string>;
        /** Array forms of entries/keys/values, for code that indexes or measures the result. */
        entriesArray(): Array<[string, string]>;
        /** The names in order, as an array (the pre-iteration `keys()` form). */
        keysArray(): string[];
        /** The values in order, as an array (the pre-iteration `values()` form). */
        valuesArray(): string[];
    }
    /** IDNA 2008 / UTS #46 ToASCII (Unicode 16.0) with CONTEXTJ/CONTEXTO rules; {transitional} selects transitional processing. By default a disallowed code point (lone surrogates included) is dropped, as before policy P15 (owner decision D8); { strict: true } makes it a TypeError instead. Every other validation failure (bad A-label, hyphen placement, CONTEXTJ/CONTEXTO, bidi, DNS length) is a TypeError in both modes. A non-boolean strict and an unknown key are a TypeError. */
    function domainToASCII(domain: string, options?: { transitional?: boolean; strict?: boolean }): string;
    /** IDNA ToUnicode: decodes punycode labels for display. */
    function domainToUnicode(domain: string, options?: { transitional?: boolean }): string;
    /** RFC 3492 encoding; input over 1024 code points is refused. */
    function punycodeEncode(text: string): string;
    /** RFC 3492 decoding; capped at 1024 UTF-8 octets (RangeError beyond), the decode-side DoS bound. */
    function punycodeDecode(text: string): string;
    /** Percent-encodes the object's own enumerable string keys into a=1&b=2; `~` stays unescaped (WHATWG form encoding writes %7E; kept deliberately). */
    function formEncode(obj: Record<string, unknown>): string;
    /** Decodes `+` as space, keeps the LAST value per key. A lone surrogate in the text and percent-decoded bytes that are not UTF-8 (encoded surrogates %ED%A0%80 included) become U+FFFD per maximal subpart, as URLSearchParams does. */
    function formDecode(text: string): Record<string, string>;
    /** encodeURIComponent plus !'()~. */
    function encodeURIComponentStrict(text: string): string;
}

/** dyna:uuid — UUID v1–v8 from the OS CSPRNG, parse/validate/compare, plus NanoID and ULID (optionally monotonic). */
declare module "dyna:uuid" {
    /** Random version-4 UUID (OS CSPRNG); unknown option keys are refused (TypeError). */
    function v4(opts?: { as?: "string" }): string;
    /** The 16 bytes a v4 string would encode, for DB-key paths. */
    function v4(opts: { as: "bytes" }): Uint8Array;
    /** Time-ordered v7 UUID (48-bit ms + RFC 9562 same-ms counter; OS CSPRNG random bytes): later ids sort after earlier ones. */
    function v7(opts?: { as?: "string" }): string;
    /** The 16 bytes a v7 string would encode (same counter logic). */
    function v7(opts: { as: "bytes" }): Uint8Array;
    /** RFC 9562 v8: the caller supplies all 16 bytes; only the version nibble and variant bits are overwritten; wrong length is a RangeError. */
    function v8(opts: { fill: ByteView }): string;
    /** -1 | 0 | 1 over the canonical 16 bytes (the order v7 ids sort in); any accepted string form works. */
    function compare(a: string, b: string): -1 | 0 | 1;
    /** MD5-based name UUID. */
    function v3(namespace: string | ByteView, name: BytesInput): string;
    /** SHA-1-based name UUID. */
    function v5(namespace: string | ByteView, name: BytesInput): string;
    /** Parses any accepted form and returns the canonical lowercase string. */
    function parse(uuid: string): string;
    /** True iff the argument is a string in an accepted form. */
    function validate(value: unknown): boolean;
    /** The version nibble; throws on a malformed string. */
    function version(uuid: string): number;
    /** "NCS", "RFC4122", "Microsoft" or "Future". */
    function variant(uuid: string): string;
    /** The 16 raw bytes of a parsed UUID. */
    function bytes(uuid: string): Uint8Array;
    /** The canonical string for exactly 16 bytes. */
    function fromBytes(bytes: ByteView): string;
    /** URL-safe ID over the default 64-symbol alphabet (OS CSPRNG); size 1..4096. */
    function NanoID(size?: number): string;
    /** The same generator over a caller-supplied alphabet of 2..256 ASCII symbols. */
    function NanoIDAlphabet(alphabet: string, size?: number): string;
    /** A 26-character Crockford base32 ULID; {monotonic: true} makes same-millisecond calls strictly ascending process-wide (default: fresh entropy per call). */
    function ULID(atMillis?: number | bigint, opts?: { monotonic?: boolean }): string;
    /** The millisecond timestamp encoded in the first 10 characters. */
    function ULIDTime(ulid: string): number;
    /** The all-zero UUID. */
    const NIL: string;
    /** The all-ones UUID. */
    const MAX: string;
    /** Predefined RFC 4122 name namespaces. */
    const NAMESPACE_DNS: string;
    const NAMESPACE_URL: string;
    const NAMESPACE_OID: string;
    const NAMESPACE_X500: string;
}

/** dyna:validate — boolean predicates for common formats: email, URL, UUID, IP, credit card, IBAN, JWT shape, base64, hex, JSON, MIME, RFC 3339, strong passwords. */
declare module "dyna:validate" {
    /** ASCII letters only. */
    function IsAlpha(text: string): boolean;
    /** ASCII letters and digits. */
    function IsAlphanumeric(text: string): boolean;
    /** Non-empty text whose every byte is below 0x80; IsAscii("") is false. */
    function IsAscii(text: string): boolean;
    /** The practical email grammar; quoted strings and comments are refused. */
    function IsEmail(text: string): boolean;
    /** Luhn check digit over 12..19 digits. */
    function IsCreditCard(text: string): boolean;
    /** Country-length check plus mod-97 over the rearranged digits. */
    function IsIBAN(text: string): boolean;
    /** RFC 1035 label grammar; an IP literal is not a domain. */
    function IsDomain(text: string): boolean;
    /** The dyna:url constructor accepts it, with a non-empty host for special schemes. */
    function IsURL(text: string): boolean;
    /** Lowercase letters, digits, single hyphens; at most 64 chars. */
    function IsSlug(text: string): boolean;
    /** The RFC 4122 canonical 8-4-4-4-12 form only. */
    function IsUUID(text: string): boolean;
    /** JWS Compact Serialization with a JSON header naming an alg; opts.algs (array) also requires that alg in the list.
     *  Unknown opts keys or a non-object opts throw TypeError; a throwing opts.algs read or length is rethrown. */
    function IsJWT(text: string, opts?: { algs?: string[] }): boolean;
    /** The dyna:semver parser accepts it. */
    function IsSemver(text: string): boolean;
    /** ITU-T E.164: optional `+`, 8..15 digits. */
    function IsE164(text: string): boolean;
    /** Strict dotted-quad: four decimal octets 0..255, no leading zeros, no whitespace. */
    function IsIPv4(text: string): boolean;
    /** RFC 4291 text grammar incl. `::` compression (once) and an embedded IPv4 tail; no zone ids. */
    function IsIPv6(text: string): boolean;
    /** IsIPv4 or IsIPv6. */
    function IsIP(text: string): boolean;
    /** Decimal 0..65535, no sign, no leading zeros; "0" is allowed (a kernel-assigned port). */
    function IsPort(text: string): boolean;
    /** RFC 4648 canonical base64: `+/` alphabet, proper padding, zero pad bits. */
    function IsBase64(text: string): boolean;
    /** RFC 4648 sec.5 base64url: the `-_` alphabet, unpadded and canonical (zero pad bits). */
    function IsBase64Url(text: string): boolean;
    /** An even-length run of hex digits -- the shape a byte string must have. */
    function IsHex(text: string): boolean;
    /** `#RGB`, `#RRGGBB` or `#RRGGBBAA`; the `#` is required. */
    function IsHexColor(text: string): boolean;
    /** JSON.parse accepts it (any JSON value; input cap 4096 bytes). */
    function IsJSON(text: string): boolean;
    /** RFC 6838 simplified: token type/subtype, optional `; name=value` parameters. */
    function IsMimeType(text: string): boolean;
    /** Full RFC 3339 date-time: `T` separator, `Z` or `+/-HH:MM` offset; the leap second `:60` is allowed. */
    function IsRFC3339(text: string): boolean;
    /** Strict ISO calendar date YYYY-MM-DD with real month/day validation (leap years included). */
    function IsDateString(text: string): boolean;
    /** Optional sign, digits, at most one decimal point; exponent notation is refused. */
    function IsNumeric(text: string): boolean;
    /** Printable ASCII with class minimums; defaults 8/1/1/1/1; symbols are printable non-alphanumeric ASCII. */
    function IsStrongPassword(text: string, opts?: {
        minLength?: number;
        minLower?: number;
        minUpper?: number;
        minDigits?: number;
        minSymbols?: number;
    }): boolean;
}

/** dyna:oauth2 — OAuth 2.0 helpers (RFC 6749/6750/7636): PKCE, state, bearer parsing, redirect/scope checks, claim-validating verifyJWT (needs the optional "crypto" module, which links OpenSSL). No IO. */
declare module "dyna:oauth2" {
    /** 43..128 unreserved chars; 32 octet CSPRNG -> 43 char base64url */
    function generateCodeVerifier(): string;
    /** True for a PKCE code_verifier (43..128 unreserved characters, RFC 7636). */
    function isValidCodeVerifier(s: string): boolean;
    /** S256 = BASE64URL(SHA256(verifier)), plain = verifier; S256 default */
    function generateCodeChallenge(verifier: string, method?: "S256" | "plain"): string;
    /** Verifies a PKCE challenge; method "S256"|"plain" (else TypeError); constant-time. */
    function verifyCodeChallenge(verifier: string, challenge: string, method: string): boolean;
    /** CSPRNG bytes -> base64url; default 32 bytes */
    function generateState(bytes?: number): string;
    /** Constant-time compare over UTF-8 strings or raw bytes; the convenience form of crypto.TimingSafeEqual. */
    function secureCompare(a: BytesInput, b: BytesInput): boolean;
    /** Client Basic auth per RFC 6749 s2.3.1: "Basic " + base64(formEncode(id:secret)), prefix included. */
    function buildClientAuthHeader(clientId: string, clientSecret?: string): string;
    /** Verifies a JWT access token: alg allowlist (`none` refused), key family, then exp/nbf/aud/iss/scope; clockSkewSec 0..300; requireExp rejects a missing exp.
     *  The signature check is dyna:crypto's JWTVerify, reached through a bridge compiled on the first call in a realm and kept as the hidden global
     *  `__oauth_jwt_verify` (non-writable, non-configurable, non-enumerable); later calls reuse it (compiling it per call was ~70% of a verify).
     *  A writable or configurable property of that name is replaced on the first call; one already of the bridge's shape is used as the bridge. */
    function verifyJWT<T = unknown>(token: string, key: BytesInput, opts: { algorithms: string[]; aud?: string | string[]; iss?: string; requiredScope?: string[]; clockSkewSec?: number; requireExp?: boolean }): T;
    /** NQCHAR scope tokens, space-delimited, per RFC 6749 A.4 */
    function parseScope(scope: string): string[];
    /** Joins scopes with single spaces. */
    function formatScope(scopes: string[]): string;
    /** "Bearer <b64token>" with b64token validation */
    function buildBearerHeader(token: string): string;
    /** The token from an `Authorization: Bearer ...` header value, or null. */
    function parseBearerHeader(header: string): string | null;
    /** headers.authorization/query/body; allowQuery/allowBody opt-in (default false) */
    function parseBearerFromRequest(req: { headers?: Record<string,string>; query?: string; body?: string; allowQuery?: boolean; allowBody?: boolean }): string | null;
    /** RFC 6750 b64token: 1*b64char *"=", no length cap; strict: at most two "=" and 4096 bytes. */
    function isValidBearerToken(token: string, opts?: { strict?: boolean }): boolean;
    /** Bearer realm, error (REQUIRED, 1*NQSCHAR per RFC 6749 A.7), error_description (A.7), error_uri (A.8, no SP), scope, all quoted; TypeError otherwise.
        strict: error limited to 1-64 token68 chars. */
    function buildWWWAuthenticate(opts: { realm?: string; error: string; errorDescription?: string; error_description?: string; errorUri?: string; error_uri?: string; scope?: string; strict?: boolean }): string;
    /** Verbatim exact except loopback port-ignore per RFC 8252; every registered entry must be a string (TypeError naming the index), wherever the match is. */
    function isValidRedirectUri(candidate: string, registered: string[]): boolean;
    /** Builds the authorization URL (response_type=code), generating `state` when absent; the endpoint must be https (loopback excepted) unless allowInsecure. */
    function buildAuthorizationUrl(opts: { authorizationEndpoint: string; clientId: string; redirectUri: string; responseType?: "code"; scope?: string; state?: string; codeChallenge?: string; codeChallengeMethod?: "S256" | "plain"; allowPlain?: boolean; allowInsecure?: boolean; extraParams?: Record<string,string> }): { url: string; state: string };
    /** Parse ?code/state/error from redirect URL (query, not fragment for code flow); refuses implicit-flow fragments, code+error, duplicate params */
    function parseAuthorizationResponse(url: string): { code?: string; state?: string; error?: string; errorDescription?: string; errorUri?: string };
    /** application/x-www-form-urlencoded body builder; every value must be a string */
    function buildTokenRequestBody(params: Record<string,string>): string;
    /** Token response per RFC 6749: access_token + token_type Bearer required, b64token access_token */
    interface TokenResponse {
        access_token: string;
        token_type: string;
        expires_in?: number;
        refresh_token?: string;
        scope?: string;
        [key: string]: unknown;
    }
    /** Parses a token endpoint JSON body into a TokenResponse. */
    function parseTokenResponse(body: string): TokenResponse;
}

/** dyna:xml — XML three ways: a tree (XMLParse), streaming SAX (push or pull), and a streaming writer; size caps are on and no DTD is processed. */
declare module "dyna:xml" {
    /** A document-tree element node. */
    interface XMLElement {
        name: string;
        attrs: Record<string, string>;
        children: (string | XMLElement)[];
    }

    /** Parses a document into a tree; `multiple: true` returns every root element. At most one DOCTYPE, before the root; only the five predefined entities exist.
     *  Limits: 256 MiB input (RangeError), 256 open elements (SyntaxError), 16 MiB per token. */
    function XMLParse(text: string, opts?: { trim?: boolean; entities?: "strict" | "keep"; multiple?: boolean }): XMLElement | XMLElement[];
    /** Serializes a node; indent 0..16 spaces; nesting beyond 256 throws. The options bag is strict (unknown key throws). */
    function XMLStringify(node: XMLElement, opts?: { indent?: number }): string;
    /** Collapses an element into a plain object keyed by element name. */
    function XMLToObject(node: XMLElement): Record<string, unknown>;

    /** A streaming document writer for output too deep or long for a tree: only the open-element stack is held; nesting to 65536. */
    class XmlWriter {
        constructor(opts?: { indent?: number });
        /** Writes `<name attrs>` (attribute values escaped) and pushes the element; invalid names throw. */
        open(name: string, attrs?: Record<string, string>): void;
        /** Appends escaped text (& < >). */
        text(s: string): void;
        /** Writes `</name>` for the innermost open element; throws when none is open. */
        close(): void;
        /** The bytes so far, WITHOUT closing anything. */
        toString(): string;
        /** Closes every open element and returns the complete document; idempotent. */
        finish(): string;
    }

    /** Streaming SAX handlers. */
    interface SAXHandlers {
        onOpen?: (name: string, attrs: Record<string, string>) => void;
        onClose?: (name: string) => void;
        onText?: (text: string) => void;
        onCData?: (text: string) => void;
        onComment?: (text: string) => void;
        onPI?: (target: string, data: string) => void;
    }
    /** One pull event: `name` on open/close/pi, `attrs` on open, `text` on text/cdata/comment/pi; `offset` is the absolute byte offset in the stream. A self-closing `<b/>` yields open then close, in push and pull form. */
    interface SAXEvent {
        event: "open" | "close" | "text" | "cdata" | "comment" | "pi";
        name?: string;
        attrs?: Record<string, string>;
        text?: string;
        offset: number;
    }
    /** Streaming SAX parser with XMLParse's document rules; total input is capped at 256 MiB (RangeError), the XMLParse limit.
     *  MODE: with a handlers object it is a PUSH parser (write() fires handlers); with none a PULL parser (write() buffers, next() returns one SAXEvent or null). */
    class SAXParser {
        /** The handlers object is also the options bag (`trim`, `entities`); omit it for the pull form, whose options come from the second argument. */
        constructor(handlers?: SAXHandlers & { trim?: boolean; entities?: "strict" | "keep" }, opts?: { entities?: "strict" | "keep" });
        /** Feeds a string or any byte view. */
        write(chunk: BytesInput): void;
        /** Finalizes the stream. Push parsers run their end-of-document checks here (trailing content throws); pull parsers defer them to the last next(). */
        end(): void;
        /** Pull form: scans forward and returns the next SAXEvent, or null while the input is drained (feed more, or the document is finished). Errors throw and are sticky. */
        next(): SAXEvent | null;
        /** Async-iterable wrapper: drives a pull parser over any sync/async iterable of chunks, yielding one SAXEvent per step. */
        static iterate(source: AsyncIterable<BytesInput> | Iterable<BytesInput>, opts?: { entities?: "strict" | "keep" }): AsyncGenerator<SAXEvent>;
    }
}

/** dyna:yaml — YAML 1.2 core schema: Parse, ParseAll, lazy ParseStream, Stringify; anchors and aliases are refused by name, never expanded. */
declare module "dyna:yaml" {
    /** Parses exactly one document. { maxDepth: 1..128 } lowers the nesting cap; { schema } accepts "core" and refuses "full" by name. A leading BOM is skipped and NUL is kept. A core-schema int (decimal, 0x, 0o) is a Number, rounded like JSON.parse beyond +-(2^53-1) (owner decision D8: lossy by default, so 9007199254740993 reads as 9007199254740992); { bigint: true } makes such an int an exact BigInt. A digit run over 4096 characters stays a plain string in both modes. A non-boolean bigint, an unknown key or a non-object options value is a TypeError. */
    function Parse(text: string, opts?: { maxDepth?: number; schema?: "core" | "full"; bigint?: boolean }): unknown;
    /** Parses every `---`-separated document into an array. Same options as Parse. */
    function ParseAll(text: string, opts?: { maxDepth?: number; schema?: "core" | "full"; bigint?: boolean }): unknown[];
    /** Serializes to YAML. { indent } 1..10 (default 2); { width } > 0 folds long strings losslessly (0 = never); { sortKeys } orders keys bytewise; { flow } emits `[a, b]`. Functions follow JSON.stringify: a function-valued property is omitted and a function in an array or at the top is null (function source is never written). A BigInt is a plain integer scalar (`-5n` -> `-5`) and parses back as the same BigInt (with { bigint: true }) up to 4096 characters; a string that reads as an int is quoted. An integer Number beyond +-(2^53-1) gets a `.0` and -0 is `-0.0`, so both parse back as the same Number. */
    function Stringify(value: unknown, opts?: {
        indent?: number;
        width?: number;
        sortKeys?: boolean;
        flow?: boolean;
    }): string;
    /** Lazy multi-document parsing: a SYNC iterator over the `---`-separated documents, one alive at a time; an error names its document and line and ends the iterator. Same options as Parse. */
    function ParseStream(text: string, opts?: { maxDepth?: number; schema?: "core" | "full"; bigint?: boolean }): Iterator<unknown>;
}
/** dyna:dataframe — in-memory columnar analytics over TypedArrays: UPPERCASE verbs for filter masks, sort, group-by, joins, windows, quantiles, CSV out.
 *  Masks are Uint8Arrays produced by predicate verbs and accepted by aggregates; string columns are dictionary-encoded. */
declare module "dyna:dataframe" {
    /** One column's description. Numeric text uses `.` regardless of the host locale; POSIX GCC/Clang only (MSVC is not a verified target). */
    interface DataFrameColumn {
        name: string;
        type: string;
    }
    /** A grouped aggregate: keys[i] pairs with values[i]. */
    interface GroupResult {
        keys: (string | number)[];
        values: Float64Array;
    }
    /** GROUP_BY_AGG result: `keys` plus one Float64Array per requested aggregate, always in the order sum, mean, count, min, max; an aggregate not requested is absent. */
    interface GroupAggResult {
        keys: (string | number)[];
        sum?: Float64Array;
        mean?: Float64Array;
        count?: Float64Array;
        min?: Float64Array;
        max?: Float64Array;
    }
    /** Grouped arrays: keys[i] pairs with the Float64Array values[i]. */
    interface GroupArrays {
        keys: (string | number)[];
        values: Float64Array[];
    }
    /** A frame instance: shape getters plus the UPPERCASE verbs. Every verb whose result depends on how equal keys or timestamps are ordered uses one specified order at every row count, so a kernel switching algorithm at a size threshold never changes a result, not even the last bit of a float sum: ties keep input row order unless the verb states its own rule (GROUP_ARRAY_SORTED puts -0 before 0; RANGE_AGG reports -0 bounds as 0). */
    interface DataFrame {
        /** Row count. */
        readonly ROWS: number;
        /** Column count. */
        readonly COLS: number;
        /** Column names in column order. */
        readonly COLUMNS: string[];
        /** Column name to a short type tag: f64 f32 i32 u32 i16 u16 i8 u8 str. */
        DTYPES(): Record<string, string>;
        /** One {name, type} entry per column, in order. */
        SCHEMA(): DataFrameColumn[];
        /** {rows, cols, dtypes, bytes, total_bytes}. */
        INFO(): { rows: number; cols: number; dtypes: Record<string, string>; bytes: Record<string, number>; total_bytes: number };
        /** {columns: {name: bytes}, total}. */
        MEMORY_USAGE(): { columns: Record<string, number>; total: number };
        /** Column name to a fresh TypedArray copy or array of strings. */
        TO_COLUMNS(): Record<string, Uint8Array | Int8Array | Uint16Array | Int16Array | Uint32Array | Int32Array | Float32Array | Float64Array | string[]>;
        /** One object per row. */
        TO_RECORDS(): Record<string, number | string>[];
        /** The TO_RECORDS array serialised; NaN and Infinity become null. */
        TO_JSON(): string;
        /** CSV text with a header row (RFC 4180 quoting); {escapeFormulas: true} prefixes `'` to cells starting with = + - @ (CSV injection); NaN prints as an empty cell. */
        TO_CSV(opts?: { escapeFormulas?: boolean }): string;
        /** Builds a frame from an array of row objects. */
        FROM_RECORDS(rows: Record<string, unknown>[]): DataFrame;
        /** A fresh frame whose columns are exact copies. */
        COPY(): DataFrame;
        /** The named columns in the order given. */
        SELECT(names: string[]): DataFrame;
        /** The complement, in column order. */
        DROP_COLUMNS(names: string[]): DataFrame;
        /** Renames columns per {old: new}. */
        RENAME(map: Record<string, string>): DataFrame;
        /** The rows where the ROWS-byte mask is nonzero. */
        FILTER(mask: Uint8Array): DataFrame;
        /** Rows [start, end), clamped and negative-indexed like Array.prototype.slice. */
        SLICE(start: number, end?: number): DataFrame;
        /** n rows without replacement via a Fisher-Yates partial shuffle. */
        SAMPLE(n: number, seed?: number): DataFrame;
        /** 1 where the column's value is in values; each value is coerced to the column's type (its error propagates); a hole or undefined entry is a TypeError naming its index. */
        ISIN(col: string, values: (number | string)[]): Uint8Array;
        /** The same shape; rows where mask is 0 become fill. */
        MASK(mask: Uint8Array, fill?: number | string): DataFrame;

        /** Reductions over (col[, mask]); masked-out rows do not contribute. */
        SUM(col: string, mask?: Uint8Array): number;
        /** Aggregates take a column name and an optional row mask (a Uint8Array from the predicate verbs). */
        MIN(col: string, mask?: Uint8Array): number | undefined;
        /** The maximum, ignoring NaN. */
        MAX(col: string, mask?: Uint8Array): number | undefined;
        /** The sum over the count. NaN on an empty selection. */
        MEAN(col: string, mask?: Uint8Array): number;
        /** The number of rows, or of nonzero mask bytes. */
        COUNT(col: string, mask?: Uint8Array): number;
        /** The product, accumulated in double for every column type. */
        PRODUCT(col: string, mask?: Uint8Array): number;
        /** The sum of `a[i] * b[i]` over the selection, via specialised same-type kernels and block-widened mixed pairs. */
        DOT_PRODUCT(a: string, b: string, mask?: Uint8Array): number;
        /** Sample (n-1) variance and standard deviation; the *_POP forms divide by n. */
        VARIANCE(col: string, mask?: Uint8Array): number;
        /** The sample standard deviation. */
        STDDEV(col: string, mask?: Uint8Array): number;
        /** The population variance (/n), defined from one row up. */
        VARIANCE_POP(col: string, mask?: Uint8Array): number;
        /** The population standard deviation. */
        STDDEV_POP(col: string, mask?: Uint8Array): number;
        /** The population skewness. */
        SKEW(col: string, mask?: Uint8Array): number;
        /** The population excess kurtosis. */
        KURTOSIS(col: string, mask?: Uint8Array): number;
        /** The sample skewness. 0 below n=3. */
        SKEW_SAMP(col: string, mask?: Uint8Array): number;
        /** The sample excess kurtosis. 0 below n=4. */
        KURT_SAMP(col: string, mask?: Uint8Array): number;
        /** Standard error of the mean. */
        SEM(col: string, mask?: Uint8Array): number;
        /** The number of NaN values in the selection. */
        COUNT_NULLS(col: string, mask?: Uint8Array): number;
        /** The sum of `w*x` over sum of `w`. A zero weight contributes nothing (it is not in the input set), and all-zero weights give NaN. */
        MEAN_WEIGHTED(valueCol: string, weightCol: string, mask?: Uint8Array): number;
        /** Exact integer sum over integer columns only (TypeError on float columns); RangeError when the total overflows a Number. */
        SUM_CHECKED(col: string, mask?: Uint8Array): number;
        /** {count, sum, mean, min, max, variance, stddev, skew, kurtosis} in one pass. */
        DESCRIBE(col: string, mask?: Uint8Array): { count: number; sum: number; mean: number; min: number; max: number; variance: number; stddev: number; skew: number; kurtosis: number };
        /** Shannon entropy in bits over the empirical value distribution. */
        ENTROPY(col: string, mask?: Uint8Array): number;
        /** The mean absolute deviation from the mean. `undefined` when nothing is selected. */
        MAD(col: string, mask?: Uint8Array): number | undefined;
        /** The median of `|x - median|`. `undefined` when nothing is selected. */
        MEDIAN_ABSOLUTE_DEVIATION(col: string, mask?: Uint8Array): number | undefined;

        /** Bitwise folds over integer columns; empty-selection identities as documented. */
        BITWISE_AND(col: string, mask?: Uint8Array): number;
        /** The bitwise OR of the selection. Identity 0. */
        BITWISE_OR(col: string, mask?: Uint8Array): number;
        /** The bitwise XOR of the selection. Identity 0. */
        BITWISE_XOR(col: string, mask?: Uint8Array): number;
        /** Each group's integer values fold as 32-bit patterns (ToUint32); the result is the unsigned value 0..4294967295. */
        GROUP_BIT_AND(key: string, val: string, mask?: Uint8Array): GroupResult;
        /** The per-group bitwise OR of the integer value column. */
        GROUP_BIT_OR(key: string, val: string, mask?: Uint8Array): GroupResult;
        /** The per-group bitwise XOR of the integer value column. */
        GROUP_BIT_XOR(key: string, val: string, mask?: Uint8Array): GroupResult;
        /** The count of DISTINCT non-negative integer values, one bit per value. */
        GROUP_BITMAP(col: string, mask?: Uint8Array): number;

        /** Positional access; n defaults to 5 and is clamped to the frame. */
        HEAD(col: string, n?: number, mask?: Uint8Array): Float64Array;
        /** The last `n` selected values, with the same clamping. */
        TAIL(col: string, n?: number, mask?: Uint8Array): Float64Array;
        /** The value of the first SELECTED row. `undefined` when nothing is selected (never NaN, so it cannot be confused with a NaN value). */
        FIRST(col: string, mask?: Uint8Array): number | undefined;
        /** The value of the last selected row. */
        LAST(col: string, mask?: Uint8Array): number | undefined;
        /** The row index of the minimum. Ties go to the FIRST occurrence, NaN values are skipped. */
        ARG_MIN(col: string, mask?: Uint8Array): number | undefined;
        /** The row index of the maximum. Ties go to the first occurrence. */
        ARG_MAX(col: string, mask?: Uint8Array): number | undefined;

        /** Mask-producing comparisons over a column. */
        GT(col: string, value: number): Uint8Array;
        /** Comparison verbs return a row mask (1 = match). */
        GE(col: string, value: number): Uint8Array;
        /** A mask that is 1 where `col[i] < value`. */
        LT(col: string, value: number): Uint8Array;
        /** A mask that is 1 where `col[i] <= value`. */
        LE(col: string, value: number): Uint8Array;
        /** Numeric equality mask; a string column throws TypeError — match strings with ISIN. */
        EQ(col: string, value: number): Uint8Array;
        /** Numeric inequality mask; same string-column refusal as EQ. */
        NE(col: string, value: number): Uint8Array;
        /** 1 where lo <= col[i] <= hi, inclusive at both ends. */
        BETWEEN(col: string, lo: number, hi: number): Uint8Array;
        /** Mask of NaN cells (NOT_NA is its complement). */
        IS_NA(col: string): Uint8Array;
        /** A mask that is 1 where the value is not NaN. */
        NOT_NA(col: string): Uint8Array;
        /** True when every mask byte is nonzero. */
        ALL(mask: Uint8Array): boolean;
        /** True when any mask byte is set. */
        ANY(mask: Uint8Array): boolean;
        /** The mask packed into ceil(ROWS/32) words, LSB first. */
        BITMASK(mask: Uint8Array): Uint32Array;
        /** The AND of the JS truthiness of the stored values (a float NaN is false, any nonzero int is true). */
        BOOL_AND(col: string, mask?: Uint8Array): boolean;
        /** The OR of the JS truthiness of the stored values. Vacuous over zero selected rows: false. */
        BOOL_OR(col: string, mask?: Uint8Array): boolean;
        /** The parity of the true count. Vacuous over zero selected rows: false. */
        BOOL_XOR(col: string, mask?: Uint8Array): boolean;
        /** 1 on the FIRST occurrence of each distinct value. */
        DROP_DUPLICATES(col: string, mask?: Uint8Array): Uint8Array;
        /** 1 where none of the named columns is NaN; no arguments: every numeric column. */
        DROP_NA(...cols: string[]): Uint8Array;

        /** Elementwise verbs return a Float64Array of ROWS entries; binary ones take a number or another COLUMN NAME (RSUB/RDIV: number only). With { out: name } (here and on CLIP, FILL_NA, ROLLING_*, EMA) the result is written into that column and the frame is returned: an existing out column must be a Float64Array column; a name the frame lacks adds a new f64 column after the others (MUL("price", "qty", { out: "rev" })), up to the 1024-column cap, and a verb that throws adds nothing. */
        ABS(col: string): Float64Array;
        ABS(col: string, opts: { out: string }): this;
        /** Each element rounded half away from zero. */
        ROUND(col: string): Float64Array;
        ROUND(col: string, opts: { out: string }): this;
        /** The largest integer <= each value. */
        FLOOR(col: string): Float64Array;
        FLOOR(col: string, opts: { out: string }): this;
        /** The smallest integer >= each value. */
        CEIL(col: string): Float64Array;
        CEIL(col: string, opts: { out: string }): this;
        /** The square root of each element. */
        SQRT(col: string): Float64Array;
        SQRT(col: string, opts: { out: string }): this;
        /** The natural logarithm of each element. */
        LOG(col: string): Float64Array;
        LOG(col: string, opts: { out: string }): this;
        /** E raised to each value. */
        EXP(col: string): Float64Array;
        EXP(col: string, opts: { out: string }): this;
        /** -1, 0 or 1 per element. */
        SIGN(col: string): Float64Array;
        SIGN(col: string, opts: { out: string }): this;
        /** Each element clamped into `[lo, hi]`. NaN elements pass through unchanged. */
        CLIP(col: string, lo: number, hi: number): Float64Array;
        CLIP(col: string, lo: number, hi: number, opts: { out: string }): this;
        /** The column with NaN replaced by `value`. A NaN `value` is legal and is a no-op. */
        FILL_NA(col: string, value: number): Float64Array;
        FILL_NA(col: string, value: number, opts: { out: string }): this;
        /** Elementwise sum; a string operand is a column REFERENCE, so ADD("a", "b") adds two columns. */
        ADD(col: string, x: number | string): Float64Array;
        ADD(col: string, x: number | string, opts: { out: string }): this;
        /** The elementwise difference. */
        SUB(col: string, x: number | string): Float64Array;
        SUB(col: string, x: number | string, opts: { out: string }): this;
        /** The elementwise product. */
        MUL(col: string, x: number | string): Float64Array;
        MUL(col: string, x: number | string, opts: { out: string }): this;
        /** The elementwise quotient. */
        DIV(col: string, x: number | string): Float64Array;
        DIV(col: string, x: number | string, opts: { out: string }): this;
        /** The elementwise power. */
        POW(col: string, x: number | string): Float64Array;
        POW(col: string, x: number | string, opts: { out: string }): this;
        /** k - col; number-only operand. */
        RSUB(col: string, k: number): Float64Array;
        RSUB(col: string, k: number, opts: { out: string }): this;
        /** k / col; number-only operand. */
        RDIV(col: string, k: number): Float64Array;
        RDIV(col: string, k: number, opts: { out: string }): this;
        /** a where the mask byte is nonzero, else b. */
        WHERE(mask: Uint8Array, a: string | number, b: string | number): Float64Array;

        /** Grouped verbs: integer keys are dense-window encoded in [0, 2^20) (else RangeError) and list every key 0..max, empty ones included; string keys list the strings the rows hold. */
        GROUP_BY_SUM(key: string, val: string, mask?: Uint8Array): GroupResult;
        /** The per-group mean. A group with no contributing row is NaN. */
        GROUP_BY_MEAN(key: string, val: string, mask?: Uint8Array): GroupResult;
        /** The per-group minimum, ignoring NaN. An empty group, or one whose values are all NaN, is NaN. */
        GROUP_BY_MIN(key: string, val: string, mask?: Uint8Array): GroupResult;
        /** The per-group maximum, ignoring NaN. An empty group, or one whose values are all NaN, is NaN. */
        GROUP_BY_MAX(key: string, val: string, mask?: Uint8Array): GroupResult;
        /** Rows per group; takes no value column. */
        GROUP_BY_COUNT(key: string, mask?: Uint8Array): GroupResult;
        /**
         * Several grouped aggregates in ONE pass over the rows. `aggs` names any of "sum", "mean", "count", "min", "max", each at most once;
         * an empty array, an unknown or repeated name, or a non-string entry is a TypeError. Each field equals the matching single verb
         * called with the same key, val and mask: sum = GROUP_BY_SUM, mean = GROUP_BY_MEAN, count = GROUP_BY_COUNT, min = GROUP_BY_MIN,
         * max = GROUP_BY_MAX, so keys follow the same rules (dense integer keys, string keys pruned to the strings the rows hold).
         * NaN: sum and mean propagate a NaN value; count counts the row; min and max ignore it, so a group whose values are all NaN
         * has NaN min and max. Empty group (no row passes the mask): sum 0, mean NaN, count 0, min NaN, max NaN. A string value column
         * is a TypeError. Summation order of GROUP_BY_SUM / MEAN / COUNT / MIN / MAX / AGG keyed by an i32 or str column over an f64 value column
         * (or COUNT alone): row order below 2 Mi rows; from 2 Mi rows with at most 65536 groups, each 2^20-row block is summed in row order and the block totals are added in block order (blocks may run on several
         * threads). The order depends only on ROWS and the group count, so results are deterministic on every machine.
         */
        GROUP_BY_AGG(key: string, val: string, aggs: ("sum" | "mean" | "count" | "min" | "max")[], mask?: Uint8Array): GroupAggResult;
        /** GROUP_BY_SUM under the name the map API uses. Identical call and result. */
        SUM_MAP(key: string, val: string, mask?: Uint8Array): GroupResult;
        /** GROUP_BY_MIN by its map name. */
        MIN_MAP(key: string, val: string, mask?: Uint8Array): GroupResult;
        /** GROUP_BY_MAX by its map name. */
        MAX_MAP(key: string, val: string, mask?: Uint8Array): GroupResult;
        /** Per-key arrays of values; the GROUP_ARRAY_* variants dedupe, sort, window or keep the last k. */
        GROUP_ARRAY(key: string, val: string, mask?: Uint8Array): GroupArrays;
        /** GROUP_ARRAY keeping only the first occurrence of each (group, value) pair. */
        GROUP_UNIQ_ARRAY(key: string, val: string, mask?: Uint8Array): GroupArrays;
        /** The per-group moving sum. O(m) block decomposition above `w` = 256, O(m*w) re-sum below, one shared threshold. */
        GROUP_ARRAY_MOVING_SUM(key: string, val: string, w?: number, mask?: Uint8Array): GroupArrays;
        /** The per-group moving average. Divides by what CONTRIBUTED, never by `w`. */
        GROUP_ARRAY_MOVING_AVG(key: string, val: string, w?: number, mask?: Uint8Array): GroupArrays;
        /** Each group's values sorted ascending in Float64Array.prototype.sort order (-0 before 0, NaN last, NaNs in row order) at every group size: radix from 64 values, a shell sort of the order-preserving bit keys below. */
        GROUP_ARRAY_SORTED(key: string, val: string, mask?: Uint8Array): GroupArrays;
        /** The last `k` rows seen per group (a circular window, O(n) instead of O(n*k)). */
        GROUP_ARRAY_LAST(key: string, val: string, k: number, mask?: Uint8Array): GroupArrays;
        /** The first k values in ROW order per group -- a deterministic head, not a random sample. */
        GROUP_ARRAY_SAMPLE(key: string, val: string, k: number, mask?: Uint8Array): GroupArrays;
        /** The values present in EVERY group. */
        GROUP_ARRAY_INTERSECT(key: string, val: string, mask?: Uint8Array): Float64Array;
        /** A dense array of size slots; later rows overwrite earlier ones. */
        GROUP_ARRAY_INSERT_AT(value: string, position: string, size: number, fill?: number, mask?: Uint8Array): Float64Array;
        /** The selected values joined in row order with `sep`. String columns join their dictionary strings, numeric columns their numbers. */
        GROUP_CONCAT(col: string, sep?: string, mask?: Uint8Array): string;
        /** One JSON object mapping each key to an array of that group's values. */
        JSON_AGG(key: string, value: string, mask?: Uint8Array): string;
        /** One JSON object mapping each key to the LAST value that appeared. */
        JSON_OBJECT_AGG(key: string, value: string, mask?: Uint8Array): string;
        /** JSON_AGG, but throws a RangeError on any non-finite value instead of serialising null. */
        JSON_AGG_STRICT(key: string, value: string, mask?: Uint8Array): string;
        /** JSON_OBJECT_AGG with the same strictness. */
        JSON_OBJECT_AGG_STRICT(key: string, value: string, mask?: Uint8Array): string;

        /** Ordering: sorting, ranking, frequency. NaN sorts last. */
        SORT(col: string, mask?: Uint8Array): Float64Array;
        /** Row indices that sort the column ascending. */
        ARG_SORT(col: string, mask?: Uint8Array): Uint32Array;
        /** Average ranks; ties share the mean of their positions. */
        RANK(col: string, mask?: Uint8Array): Float64Array;
        /** Ranks counting distinct values. */
        DENSE_RANK(col: string, mask?: Uint8Array): Float64Array;
        /** (min rank - 1) / (n - 1); a single valued row is 0. NaN rows stay NaN. */
        PERCENT_RANK(col: string, mask?: Uint8Array): Float64Array;
        /** SQL NTILE; first n % buckets tiles take one extra row. */
        NTILE(col: string, buckets: number, mask?: Uint8Array): Float64Array;
        /** The `k` largest values in descending order. A `k` larger than the selection clamps. */
        N_LARGEST(col: string, k: number, mask?: Uint8Array): Float64Array;
        /** The `k` smallest values in ascending order. */
        N_SMALLEST(col: string, k: number, mask?: Uint8Array): Float64Array;
        /** Distinct values in first-seen order; strings for a string column. */
        UNIQUE(col: string, mask?: Uint8Array): Float64Array | string[];
        /** The count of distinct values PRESENT in the selection (a string column's dictionary may hold more). */
        N_UNIQUE(col: string, mask?: Uint8Array): number;
        /** Exact distinct count, or n+1 meaning "more than n"; numeric columns only. */
        UNIQ_UP_TO(col: string, n: number, mask?: Uint8Array): number;
        /** Distinct values with their counts (TOP_K keeps the k most frequent). */
        VALUE_COUNTS(col: string, mask?: Uint8Array): GroupResult;
        /** The `k` most FREQUENT values (the frequency question, not the magnitude one). */
        TOP_K(col: string, k: number, mask?: Uint8Array): GroupResult;
        /** The most frequent value; ties go to the first in row order. */
        MODE(col: string, mask?: Uint8Array): number | string | undefined;
        /** Sketch-based approximate distinct count. */
        APPROX_COUNT_DISTINCT(col: string, mask?: Uint8Array): number;
        /** A Space-Saving estimate of the `k` most frequent values. */
        APPROX_TOP_K(col: string, k: number, mask?: Uint8Array): GroupResult;
        /** Ranks by summed weight, not count. */
        APPROX_TOP_SUM(col: string, weightCol: string, k: number, mask?: Uint8Array): GroupResult;
        /** The `k` values with the largest summed weight (or frequency when the weight column is omitted). Each key's weights are summed in row order, so the sums are deterministic to the last bit; -0 and 0 are one key, reported as 0. */
        TOP_K_WEIGHTED(col: string, weightCol: string | undefined, k: number, mask?: Uint8Array): GroupResult;
        /** The value holding strictly more than half the total weight. */
        ANY_HEAVY(col: string, weightCol?: string, mask?: Uint8Array): number | undefined;
        /** MinHash Jaccard estimate between two columns. */
        APPROX_SIMILARITY(a: string, b: string, mask?: Uint8Array): number;

        /** Quantiles: select rather than sort. */
        QUANTILE(col: string, q: number, mask?: Uint8Array): number | undefined;
        /** Interpolated quantile for q in [0, 1]; PERCENTILE_DISC returns an actual element. */
        PERCENTILE_CONT(col: string, q: number, mask?: Uint8Array): number | undefined;
        /** The order statistic at `ceil(q * n)`, so the answer IS a value the column holds. */
        PERCENTILE_DISC(col: string, q: number, mask?: Uint8Array): number | undefined;
        /** QUANTILE at 0.5. Pandas-style NaN for an empty selection. */
        MEDIAN(col: string, mask?: Uint8Array): number | undefined;
        /** The order statistic at `floor(q * (n-1))`, never interpolated. */
        QUANTILE_EXACT_LOW(col: string, q: number, mask?: Uint8Array): number | undefined;
        /** The order statistic at `ceil(q * (n-1))`, never interpolated. */
        QUANTILE_EXACT_HIGH(col: string, q: number, mask?: Uint8Array): number | undefined;
        /** Many interpolating quantiles from ONE gather. */
        QUANTILES(col: string, qs: number[], mask?: Uint8Array): Float64Array;
        /** Many approximate quantiles off ONE t-digest. Equal values enter the digest in row order (-0 and 0 tie) at every size, so the result is deterministic to the last bit. */
        QUANTILES_TDIGEST(col: string, qs: number[], mask?: Uint8Array): Float64Array;
        /** One t-digest quantile. `undefined` for no values. Equal values enter the digest in row order, as in QUANTILES_TDIGEST. */
        APPROX_PERCENTILE(col: string, q: number, mask?: Uint8Array): number | undefined;
        /** The value at which cumulative weight first reaches `q` of the total, so a row weighing three counts as three rows. Equal values accumulate their weights in row order; -0 and 0 are one value, reported as 0. */
        QUANTILE_EXACT_WEIGHTED(col: string, weightCol: string, q: number, mask?: Uint8Array): number | undefined;
        /** The approximate form in bounded memory. Equal values enter the digest in row order with their weights (-0 and 0 tie), so the result is deterministic to the last bit. */
        QUANTILE_TDIGEST_WEIGHTED(col: string, weightCol: string, q: number, mask?: Uint8Array): number | undefined;
        /** Equal-width bins over the observed range; edges has bins+1 entries. */
        HISTOGRAM(col: string, bins: number, mask?: Uint8Array): { edges: Float64Array; counts: Float64Array };
        /** The same bins with counts divided by the number of contributing rows. */
        HISTOGRAM_NORMALIZED(col: string, bins: number, mask?: Uint8Array): { edges: Float64Array; counts: Float64Array };

        /** Scans: windowed and sequential verbs over exactly ROWS entries. */
        CUM_SUM(col: string, mask?: Uint8Array): Float64Array;
        /** The running product (identity 1). */
        CUM_PROD(col: string, mask?: Uint8Array): Float64Array;
        /** The running maximum (identity -Infinity). */
        CUM_MAX(col: string, mask?: Uint8Array): Float64Array;
        /** The running minimum (identity +Infinity). */
        CUM_MIN(col: string, mask?: Uint8Array): Float64Array;
        /** out[i] = col[i - periods]; the vacated head/tail is NaN. */
        SHIFT(col: string, periods?: number): Float64Array;
        /** Difference from the row `periods` earlier (default 1). */
        DIFF(col: string, periods?: number): Float64Array;
        /** Rolling-window verbs over the trailing `w` rows. */
        ROLLING_SUM(col: string, w: number, mask?: Uint8Array): Float64Array;
        ROLLING_SUM(col: string, w: number, mask: Uint8Array, opts: { out: string }): this;
        ROLLING_SUM(col: string, w: number, opts: { out: string }): this;
        /** The trailing window mean. Divides by what CONTRIBUTED, never by `w`. */
        ROLLING_MEAN(col: string, w: number, mask?: Uint8Array): Float64Array;
        ROLLING_MEAN(col: string, w: number, mask: Uint8Array, opts: { out: string }): this;
        ROLLING_MEAN(col: string, w: number, opts: { out: string }): this;
        /** The trailing window minimum. Monotonic-deque O(n); NaN never enters the deque. */
        ROLLING_MIN(col: string, w: number, mask?: Uint8Array): Float64Array;
        ROLLING_MIN(col: string, w: number, mask: Uint8Array, opts: { out: string }): this;
        ROLLING_MIN(col: string, w: number, opts: { out: string }): this;
        /** The trailing window maximum. Same deque. */
        ROLLING_MAX(col: string, w: number, mask?: Uint8Array): Float64Array;
        ROLLING_MAX(col: string, w: number, mask: Uint8Array, opts: { out: string }): this;
        ROLLING_MAX(col: string, w: number, opts: { out: string }): this;
        /** The sample variance (ddof=1) per window, two passes, never a sum of squares around an uncentred mean. */
        ROLLING_VAR(col: string, w: number, mask?: Uint8Array): Float64Array;
        ROLLING_VAR(col: string, w: number, mask: Uint8Array, opts: { out: string }): this;
        ROLLING_VAR(col: string, w: number, opts: { out: string }): this;
        /** The sqrt of the rolling variance. */
        ROLLING_STD(col: string, w: number, mask?: Uint8Array): Float64Array;
        ROLLING_STD(col: string, w: number, mask: Uint8Array, opts: { out: string }): this;
        ROLLING_STD(col: string, w: number, opts: { out: string }): this;
        /** Exponential moving average; alpha in (0, 1]. */
        EMA(col: string, alpha: number, mask?: Uint8Array): Float64Array;
        EMA(col: string, alpha: number, mask: Uint8Array, opts: { out: string }): this;
        EMA(col: string, alpha: number, opts: { out: string }): this;
        /** `(x[i] - x[i-p]) / x[i-p]`. A zero previous value gives +/-Inf (the honest answer); 0/0 stays NaN. */
        PCT_CHANGE(col: string, periods?: number, mask?: Uint8Array): Float64Array;
        PCT_CHANGE(col: string, periods: number, mask: Uint8Array, opts: { out: string }): this;
        PCT_CHANGE(col: string, periods: number, opts: { out: string }): this;
        PCT_CHANGE(col: string, opts: { out: string }): this;
        /** `(x - mean) / sample stddev`, so it composes with STDDEV. */
        ZSCORE(col: string, mask?: Uint8Array): Float64Array;
        ZSCORE(col: string, mask: Uint8Array, opts: { out: string }): this;
        ZSCORE(col: string, opts: { out: string }): this;
        /** Sum of positive consecutive differences. */
        DELTA_SUM(col: string, mask?: Uint8Array): number;
        /** DELTA_SUM in timestamp order, not row order. Rows with equal timestamps are taken in row order (a stable sort at every size), so the result is deterministic. */
        DELTA_SUM_TIMESTAMP(valueCol: string, timeCol: string, mask?: Uint8Array): number;

        /** Pairwise statistics; regression verbs take (y, x). */
        COV_POP(a: string, b: string, mask?: Uint8Array): number;
        /** The sample covariance (n-1). NaN below two selected rows. */
        COV_SAMP(a: string, b: string, mask?: Uint8Array): number;
        /** Pearson correlation of two columns; NaN on zero variance. */
        CORR(a: string, b: string, mask?: Uint8Array): number;
        /** Least-squares regression of y on x (the SQL REGR_* family). */
        REGR_SLOPE(y: string, x: string, mask?: Uint8Array): number;
        /** The regression intercept. */
        REGR_INTERCEPT(y: string, x: string, mask?: Uint8Array): number;
        /** The squared correlation, clamped to 1.0. A constant `y` gives NaN (SQL's special case of 1 is not followed). */
        REGR_R2(y: string, x: string, mask?: Uint8Array): number;
        /** The mean of the x column, under the (y, x) naming. */
        REGR_AVG_X(y: string, x: string, mask?: Uint8Array): number;
        /** The mean of the y column. */
        REGR_AVG_Y(y: string, x: string, mask?: Uint8Array): number;
        /** The number of selected rows. */
        REGR_COUNT(x: string, y: string, mask?: Uint8Array): number;
        /** The sum of (x - mean x)^2. Note the argument order: SXX is the INDEPENDENT column's sum of squares. */
        REGR_SXX(y: string, x: string, mask?: Uint8Array): number;
        /** The sum of (y - mean y)^2. */
        REGR_SYY(y: string, x: string, mask?: Uint8Array): number;
        /** The sum of (y - mean y)(x - mean x). */
        REGR_SXY(y: string, x: string, mask?: Uint8Array): number;
        /** Spearman: Pearson over the average ranks. */
        RANK_CORR(x: string, y: string, mask?: Uint8Array): number;
        /** The n*n row-major correlation matrix. */
        CORR_MATRIX(cols: string[], mask?: Uint8Array): { columns: string[]; matrix: Float64Array; n: number };
        /** The covariance matrix over the same moments as COV_SAMP. */
        COV_MATRIX(cols: string[], mask?: Uint8Array): { columns: string[]; matrix: Float64Array; n: number };
        /** Change per unit time across the WHOLE selection. */
        RATE(valueCol: string, timeCol: string, mask?: Uint8Array): number;
        /** The most recent interval only. */
        IRATE(valueCol: string, timeCol: string, mask?: Uint8Array): number;
        /** The slope joining the leftmost and rightmost points by x value. */
        BOUNDING_RATIO(x: string, y: string, mask?: Uint8Array): number;
        /** Exponentially time-decayed aggregates over a time column with decay constant `tau`. */
        EXPONENTIAL_TIME_DECAYED_AVG(value: string, time: string, tau: number, mask?: Uint8Array): number | undefined;
        /** The weighted sum. */
        EXPONENTIAL_TIME_DECAYED_SUM(value: string, time: string, tau: number, mask?: Uint8Array): number | undefined;
        /** The decayed row count (the weight denominator; the value is ignored). */
        EXPONENTIAL_TIME_DECAYED_COUNT(value: string, time: string, tau: number, mask?: Uint8Array): number | undefined;
        /** The largest weighted value. */
        EXPONENTIAL_TIME_DECAYED_MAX(value: string, time: string, tau: number, mask?: Uint8Array): number | undefined;
        /** Half-open [lo, hi) ranges merged into their union. -0 and 0 are one bound, reported as 0, so the result does not depend on row order. */
        RANGE_AGG(loCol: string, hiCol: string, mask?: Uint8Array): { starts: Float64Array; ends: Float64Array };
        /** The interval common to ALL ranges. */
        RANGE_INTERSECT_AGG(loCol: string, hiCol: string, mask?: Uint8Array): { start: number; end: number } | undefined;

        /** Reshape; produced frames carry fresh copies of the data. */
        JOIN(other: DataFrame, leftKey: string, rightKey: string, how?: "inner" | "left" | "right" | "outer"): DataFrame;
        /** As-of join: each left row takes the right row with the largest rightTime <= leftTime (NaN fill); both time columns integer and ascending. */
        ASOF_JOIN(other: DataFrame, leftTime: string, rightTime: string): DataFrame;
        /** Appends the rows of another frame with the same columns. */
        CONCAT(other: DataFrame): DataFrame;
        /** Buckets a sorted numeric time column; agg sum|mean|min|max|count. */
        RESAMPLE(timeCol: string, interval: number, agg?: "sum" | "mean" | "min" | "max" | "count"): DataFrame;
        /** One row per distinct index value, one column per distinct key value. */
        PIVOT(index: string, columns: string, values: string, agg?: string): DataFrame;
        /** Long form: each (row, valueVar) pair becomes one output row. */
        MELT(idVars: string[], valueVars: string[]): DataFrame;
    }


    /** Brand check: true only for frames this engine built, never for look-alike objects. */
    function isDataFrame(v: unknown): boolean;
    /** Builds a frame from { name: TypedArray | string[] }. ZERO-COPY: TypedArray columns are ALIASED, not copied; `number[]` columns are stored as STRINGS. */
    const DataFrame: {
        /** A column may also be a dictionary {codes: Int32Array, values: string[]} (the shape of dyna:csv CsvIndex.column(c, "dict")): it becomes a string column with no per-row string; codes are copied, values must be distinct strings (TypeError) and every code must index one (RangeError). */
        new (columns: Record<string, Uint8Array | Int8Array | Uint16Array | Int16Array | Uint32Array | Int32Array | Float32Array | Float64Array | number[] | string[] | { codes: Int32Array; values: string[] }>): DataFrame;
    };
}
// ---- std / os compatibility modules, WHATWG globals, and core prototype extensions ----

/** dyna:docdb — embedded in-memory MongoDB-semantics store (query, update, index, aggregate, change streams) over BSON
 *  arenas; no wire protocol; one JS thread owns a Database, persist/load/large aggregates run on the pool. */
declare module "dyna:docdb" {
    /** The same class as dyna:serialize's ObjectId. */
    class ObjectId {
        /** No argument generates a fresh id; a 24-hex string, 12 bytes or an ObjectId copies one. */
        constructor(id?: string | ByteView | ObjectId);
        toHexString(): string;
        /** True for an ObjectId or hex string naming the same 12 bytes. */
        equals(other: ObjectId | string): boolean;
        /** Embedded creation time, second precision. */
        getTimestamp(): Date;
    }

    /** A value a document field can hold (dyna:serialize BSON mapping: int32-valued numbers store as int32, BigInt as int64). */
    type DocValue = string | number | bigint | boolean | null | Date | RegExp | Uint8Array | ObjectId
        | import("dyna:serialize").Timestamp | import("dyna:serialize").Decimal128 | import("dyna:serialize").Binary
        | import("dyna:serialize").MinKey | import("dyna:serialize").MaxKey | readonly DocValue[] | { [key: string]: DocValue | undefined };
    /** Any document; the default schema of an untyped Collection. */
    type Doc = { [key: string]: DocValue | undefined };
    /** A document as stored: _id is always present (an ObjectId unless the schema fixes its type) and stored first. */
    type DocWithId<T> = T extends { _id: infer I } ? T & { _id: I } : T & { _id: ObjectId };
    /** An insertable document: _id may be left out (an ObjectId is generated) unless the schema fixes a non-ObjectId _id. */
    type DocInsert<T> = T extends { _id: infer I } ? ObjectId extends I ? Omit<T, "_id"> & { _id?: I } : T : T & { _id?: ObjectId };

    type DocPrev = [never, 0, 1, 2, 3, 4];
    type DocElem<V> = V extends readonly (infer E)[] ? E : never;
    type DocIsObj<V> = V extends Date | RegExp | Uint8Array | ObjectId | ByteView | readonly unknown[] ? false : V extends object ? true : false;
    /** Every dotted path of T up to 5 levels, through arrays (element fields and numeric indexes, as MongoDB). */
    type DocPaths<T, D extends number = 5> = [D] extends [never] ? never : string extends keyof T ? string : T extends readonly (infer E)[]
        ? `${number}` | DocPaths<E, D> | `${number}.${DocPaths<E, DocPrev[D]>}`
        : DocIsObj<T> extends true ? { [K in keyof T & string]-?: K | (DocSub<NonNullable<T[K]>, DocPrev[D]> extends infer S extends string ? [S] extends [never] ? never : `${K}.${S}` : never) }[keyof T & string] : never;
    type DocSub<V, D extends number> = V extends readonly (infer E)[] ? `${number}` | DocPaths<E, D> | `${number}.${DocPaths<E, DocPrev[D]>}` : DocIsObj<V> extends true ? DocPaths<V, D> : never;
    /** The value at a dotted path; through an array it is the element value (any element may match). */
    type DocPathValue<T, P extends string> = T extends readonly (infer E)[]
        ? P extends `${number}` ? E : P extends `${number}.${infer R}` ? DocPathValue<E, R> : DocPathValue<E, P>
        : P extends `${infer K}.${infer R}` ? K extends keyof T ? DocPathValue<NonNullable<T[K]>, R> | (undefined extends T[K] ? undefined : never) : unknown
        : P extends keyof T ? T[P] : unknown;
    /** Paths whose value is numeric (for $inc, $mul). */
    type DocNumericPaths<T> = { [P in DocPaths<T>]: NonNullable<DocPathValue<T, P>> extends number | bigint ? P : never }[DocPaths<T>];
    /** Paths whose value is an array (for $push, $addToSet, $pull, $pop). */
    type DocArrayPaths<T> = { [P in DocPaths<T>]: NonNullable<DocPathValue<T, P>> extends readonly unknown[] ? P : never }[DocPaths<T>];

    /** BSON type names and numbers accepted by $type (query and expression). */
    type DocBsonType = "double" | "string" | "object" | "array" | "binData" | "undefined" | "objectId" | "bool" | "date" | "null"
        | "regex" | "dbPointer" | "javascript" | "symbol" | "javascriptWithScope" | "int" | "timestamp" | "long" | "decimal"
        | "minKey" | "maxKey" | "number" | 1 | 2 | 3 | 4 | 5 | 6 | 7 | 8 | 9 | 10 | 11 | 12 | 13 | 14 | 15 | 16 | 17 | 18 | 19 | -1 | 127;
    /** Query operators on one field. Range operators compare in BSON type order ($gt 5 never matches a string). */
    type DocFieldOps<V> = {
        $eq?: V | DocElem<V>; $ne?: V | DocElem<V> | null;
        $gt?: V | DocElem<V>; $gte?: V | DocElem<V>; $lt?: V | DocElem<V>; $lte?: V | DocElem<V>;
        $in?: readonly (V | DocElem<V> | RegExp | null)[]; $nin?: readonly (V | DocElem<V> | RegExp | null)[];
        $exists?: boolean; $type?: DocBsonType | readonly DocBsonType[];
        $regex?: [Extract<V | DocElem<V>, string>] extends [never] ? never : RegExp | string; $options?: string;
        $mod?: [Extract<V | DocElem<V>, number | bigint>] extends [never] ? never : readonly [divisor: number, remainder: number];
        $size?: [DocElem<V>] extends [never] ? never : number;
        $all?: [DocElem<V>] extends [never] ? never : readonly (DocElem<V> | { $elemMatch: DocFilter<DocElem<V>> })[];
        $elemMatch?: [DocElem<V>] extends [never] ? never : DocIsObj<DocElem<V>> extends true ? DocFilter<DocElem<V>> : DocFieldOps<DocElem<V>>;
        $not?: DocFieldOps<V> | RegExp;
    };
    /** A condition on one field: a value (array fields also match one element), a RegExp for strings, or operators. */
    type DocCondition<V> = V | DocElem<V> | (Extract<V | DocElem<V>, string> extends never ? never : RegExp) | DocFieldOps<V> | null;
    /** A query filter over T: dotted paths with conditions plus $and/$or/$nor; an undefined value is skipped. Nesting > 200 levels is RangeError. */
    type DocFilter<T = Doc> = { [P in DocPaths<DocWithId<T>>]?: DocCondition<DocPathValue<DocWithId<T>, P>> } & {
        $and?: readonly DocFilter<T>[]; $or?: readonly DocFilter<T>[]; $nor?: readonly DocFilter<T>[]; $comment?: string;
    };

    type DocPositional<T> = `${DocArrayPaths<T>}.$` | `${DocArrayPaths<T>}.$[]` | `${DocArrayPaths<T>}.$.${string}` | `${DocArrayPaths<T>}.$[].${string}`;
    type DocSetMap<T> = { [P in DocPaths<T>]?: DocPathValue<T, P> } & { [P in DocPositional<T>]?: DocValue };
    type DocPushMod<E> = E | { $each: readonly E[]; $position?: number; $slice?: number; $sort?: 1 | -1 | { [k: string]: 1 | -1 } };
    /** Update operators ($ = first matched array position, $[] = every element; filtered $[<id>] is refused). Fields an
     *  update adds are appended in lexicographic path order; int32 overflow widens to int64, int64 to double. */
    type DocUpdate<T = Doc> = {
        $set?: DocSetMap<T>; $setOnInsert?: DocSetMap<T>;
        $unset?: { [P in DocPaths<T> | DocPositional<T>]?: "" | 1 | true };
        $inc?: { [P in DocNumericPaths<T>]?: number | bigint }; $mul?: { [P in DocNumericPaths<T>]?: number | bigint };
        $min?: DocSetMap<T>; $max?: DocSetMap<T>;
        $rename?: { [P in DocPaths<T>]?: string };
        $push?: { [P in DocArrayPaths<T>]?: DocPushMod<DocElem<NonNullable<DocPathValue<T, P>>>> };
        $addToSet?: { [P in DocArrayPaths<T>]?: DocElem<NonNullable<DocPathValue<T, P>>> | { $each: readonly DocElem<NonNullable<DocPathValue<T, P>>>[] } };
        $pull?: { [P in DocArrayPaths<T>]?: DocCondition<DocElem<NonNullable<DocPathValue<T, P>>>> | DocFilter<DocElem<NonNullable<DocPathValue<T, P>>>> };
        $pullAll?: { [P in DocArrayPaths<T>]?: readonly DocElem<NonNullable<DocPathValue<T, P>>>[] };
        $pop?: { [P in DocArrayPaths<T>]?: 1 | -1 };
        $currentDate?: { [P in DocPaths<T>]?: true | { $type: "date" | "timestamp" } };
    };

    /** Projection: all-inclusion or all-exclusion (only _id may be excluded from an inclusion; mixing is ProjectionMixed 31254). */
    type DocProjection<T = Doc> = { [P in DocPaths<DocWithId<T>>]?: 0 | 1 | boolean };
    type DocTop<P> = P extends `${infer K}.${string}` ? K : P;
    type DocIncluded<Pr> = { [K in keyof Pr]: Pr[K] extends 1 | true ? K : never }[keyof Pr] & string;
    type DocExcluded<Pr> = { [K in keyof Pr]: Pr[K] extends 0 | false ? K : never }[keyof Pr] & string;
    /** The document shape a projection yields: an inclusion keeps the named fields (+ _id unless excluded), an exclusion drops them. */
    type DocProjected<T, Pr> = [DocIncluded<Pr>] extends [never]
        ? Omit<DocWithId<T>, Extract<DocExcluded<Pr>, keyof DocWithId<T>>>
        : Pick<DocWithId<T>, Extract<DocTop<DocIncluded<Pr>> | (Pr extends { _id: 0 | false } ? never : "_id"), keyof DocWithId<T>>>;
    /** Sort or index key pattern: dotted paths with 1 (ascending) or -1 (descending); an index takes up to 32 fields. */
    type DocSort<T = Doc> = { [P in DocPaths<DocWithId<T>>]?: 1 | -1 };

    /** find/findOne/count options; skip/limit are non-negative integers (limit 0 = no limit). */
    interface FindOptions<T = Doc, Pr extends DocProjection<T> = DocProjection<T>> {
        projection?: Pr;
        sort?: DocSort<T>;
        skip?: number;
        limit?: number;
    }
    interface WriteResult { acknowledged: true }
    interface UpdateResult<T = Doc> extends WriteResult {
        matchedCount: number;
        modifiedCount: number;
        upsertedCount: number;
        upsertedId: DocWithId<T>["_id"] | null;
    }
    /** bulkWrite counts; upsertedIds maps operation index to the new _id. */
    interface BulkWriteResult<T = Doc> extends WriteResult {
        insertedCount: number; matchedCount: number; modifiedCount: number; deletedCount: number; upsertedCount: number;
        upsertedIds: Record<number, DocWithId<T>["_id"]>;
    }
    /** One bulkWrite operation (MongoDB driver shape). */
    type DocBulkOp<T = Doc> =
        | { insertOne: { document: DocInsert<T> } }
        | { updateOne: { filter: DocFilter<T>; update: DocUpdate<T>; upsert?: boolean } }
        | { updateMany: { filter: DocFilter<T>; update: DocUpdate<T>; upsert?: boolean } }
        | { replaceOne: { filter: DocFilter<T>; replacement: DocInsert<T>; upsert?: boolean } }
        | { deleteOne: { filter: DocFilter<T> } }
        | { deleteMany: { filter: DocFilter<T> } };
    /** An index spec as listIndexes reports it (_id_ first). */
    interface DocIndexSpec<T = Doc> {
        v: 2; key: DocSort<T>; name: string;
        unique?: boolean; sparse?: boolean; partialFilterExpression?: DocFilter<T>;
    }
    /** explain() output; winningPlan.stage is one of the planner stages. */
    interface DocExplain {
        queryPlanner: {
            namespace: string; parsedQuery: Doc; rejectedPlans: DocPlan[];
            winningPlan: DocPlan;
        };
        executionStats: { nReturned: number; totalKeysExamined: number; totalDocsExamined: number; executionTimeMicros: number };
    }
    interface DocPlan {
        stage: "IDHACK" | "EOF" | "COLLSCAN" | "SORT" | "FETCH" | "IXSCAN" | "AND_SORTED" | "OR";
        inputStage?: DocPlan; inputStages?: DocPlan[]; indexName?: string; keyPattern?: Doc; [detail: string]: unknown;
    }

    /** Error names and codes (MongoDB numbering; 1000xx are dyna:docdb's own). */
    interface DocErrorCodes {
        InternalError: 1; BadValue: 2; FailedToParse: 9; TypeMismatch: 14; NamespaceNotFound: 26; IndexNotFound: 27;
        PathNotViable: 28; ConflictingUpdateOperators: 40; NamespaceExists: 48; ImmutableField: 66; InvalidOptions: 72;
        IndexOptionsConflict: 85; IndexKeySpecsConflict: 86; DocumentValidationFailure: 121; ExceededMemoryLimit: 146;
        CannotIndexParallelArrays: 171; ChangeStreamHistoryLost: 286; BSONObjectTooLarge: 10334; DuplicateKey: 11000;
        PathCollision: 31250; ProjectionMixed: 31254; InvalidSnapshotFile: 100001; SnapshotIOError: 100002; RegexTimeout: 100003;
    }
    type DocErrorName = keyof DocErrorCodes;
    /** Every database-level failure; bulkWrite/insertMany failures also carry writeErrors and the counts that applied.
     *  A $regex runs under a 100M backtracking-step budget (RegexTimeout); a result document over 16 MiB is 10334. */
    class DocDBError extends Error {
        constructor(message?: string, code?: number);
        readonly code: DocErrorCodes[DocErrorName] | 40324 | 168 | 16608 | (number & {});
        readonly codeName: DocErrorName | "UnknownError";
        readonly writeErrors?: { index: number; code: number; codeName: DocErrorName | "UnknownError"; errmsg: string }[];
        readonly result?: Partial<BulkWriteResult>;
    }

    /** "$path" field reference to a dotted path of T. */
    type DocFieldRef<T> = `$${DocPaths<T>}`;
    /** Aggregation variables. */
    type DocVar = "$$ROOT" | "$$CURRENT" | "$$REMOVE" | "$$NOW" | `$$${string}`;
    type DocA<T> = DocExpr<T>;
    /** An aggregation expression over input documents T (operator set of dd-expr.c; date parts are UTC). */
    type DocExpr<T> =
        | DocFieldRef<T> | DocVar | string | number | bigint | boolean | null | Date | ObjectId
        | { $literal: unknown }
        | { $add: readonly DocA<T>[] } | { $subtract: readonly [DocA<T>, DocA<T>] } | { $multiply: readonly DocA<T>[] }
        | { $divide: readonly [DocA<T>, DocA<T>] } | { $mod: readonly [DocA<T>, DocA<T>] } | { $pow: readonly [DocA<T>, DocA<T>] }
        | { $abs: DocA<T> } | { $ceil: DocA<T> } | { $floor: DocA<T> } | { $sqrt: DocA<T> } | { $round: DocA<T> | readonly [DocA<T>, DocA<T>] }
        | { $eq: readonly [DocA<T>, DocA<T>] } | { $ne: readonly [DocA<T>, DocA<T>] } | { $gt: readonly [DocA<T>, DocA<T>] }
        | { $gte: readonly [DocA<T>, DocA<T>] } | { $lt: readonly [DocA<T>, DocA<T>] } | { $lte: readonly [DocA<T>, DocA<T>] }
        | { $cmp: readonly [DocA<T>, DocA<T>] }
        | { $and: readonly DocA<T>[] } | { $or: readonly DocA<T>[] } | { $not: DocA<T> | readonly [DocA<T>] }
        | { $cond: readonly [DocA<T>, DocA<T>, DocA<T>] | { if: DocA<T>; then: DocA<T>; else: DocA<T> } }
        | { $ifNull: readonly DocA<T>[] }
        | { $switch: { branches: readonly { case: DocA<T>; then: DocA<T> }[]; default?: DocA<T> } }
        | { $concat: readonly DocA<T>[] } | { $toLower: DocA<T> } | { $toUpper: DocA<T> } | { $strLenCP: DocA<T> }
        | { $substr: readonly [DocA<T>, DocA<T>, DocA<T>] } | { $substrCP: readonly [DocA<T>, DocA<T>, DocA<T>] }
        | { $split: readonly [DocA<T>, DocA<T>] } | { $trim: { input: DocA<T>; chars?: DocA<T> } }
        | { $size: DocA<T> } | { $arrayElemAt: readonly [DocA<T>, DocA<T>] } | { $first: DocA<T> } | { $last: DocA<T> }
        | { $in: readonly [DocA<T>, DocA<T>] } | { $concatArrays: readonly DocA<T>[] } | { $isArray: DocA<T> | readonly [DocA<T>] }
        | { $slice: readonly [DocA<T>, DocA<T>] | readonly [DocA<T>, DocA<T>, DocA<T>] }
        | { $map: { input: DocA<T>; as?: string; in: DocA<any> } } | { $filter: { input: DocA<T>; as?: string; cond: DocA<any>; limit?: DocA<T> } }
        | { $sum: DocA<T> | readonly DocA<T>[] } | { $avg: DocA<T> | readonly DocA<T>[] }
        | { $min: DocA<T> | readonly DocA<T>[] } | { $max: DocA<T> | readonly DocA<T>[] }
        | { $mergeObjects: DocA<T> | readonly DocA<T>[] } | { $objectToArray: DocA<T> }
        | { $type: DocA<T> } | { $toString: DocA<T> } | { $toInt: DocA<T> } | { $toLong: DocA<T> } | { $toDouble: DocA<T> } | { $toBool: DocA<T> }
        | { $year: DocA<T> } | { $month: DocA<T> } | { $dayOfMonth: DocA<T> } | { $hour: DocA<T> } | { $minute: DocA<T> }
        | { $second: DocA<T> } | { $millisecond: DocA<T> } | { $dayOfWeek: DocA<T> } | { $dayOfYear: DocA<T> }
        | { readonly [field: string]: DocA<T> };
    /** The result type of expression E over T (int+int stays int; $divide and $avg are double). */
    type DocExprType<T, E> =
        E extends "$$ROOT" | "$$CURRENT" ? T : E extends "$$NOW" ? Date : E extends "$$REMOVE" ? undefined
        : E extends `$$${string}` ? unknown
        : E extends `$${infer P}` ? (P extends DocPaths<T> ? DocPathValue<T, P> : unknown)
        : E extends string | number | bigint | boolean | null | Date | ObjectId ? E
        : E extends { $literal: infer L } ? L
        : E extends { $add: any } | { $subtract: any } | { $multiply: any } | { $divide: any } | { $mod: any } | { $pow: any }
            | { $abs: any } | { $ceil: any } | { $floor: any } | { $sqrt: any } | { $round: any } | { $size: any } | { $strLenCP: any }
            | { $cmp: any } | { $sum: any } | { $avg: any } | { $toInt: any } | { $toDouble: any }
            | { $year: any } | { $month: any } | { $dayOfMonth: any } | { $hour: any } | { $minute: any } | { $second: any }
            | { $millisecond: any } | { $dayOfWeek: any } | { $dayOfYear: any } ? number
        : E extends { $toLong: any } ? number | bigint
        : E extends { $eq: any } | { $ne: any } | { $gt: any } | { $gte: any } | { $lt: any } | { $lte: any }
            | { $and: any } | { $or: any } | { $not: any } | { $in: any } | { $isArray: any } | { $toBool: any } ? boolean
        : E extends { $concat: any } | { $toLower: any } | { $toUpper: any } | { $substr: any } | { $substrCP: any }
            | { $trim: any } | { $toString: any } | { $type: any } ? string
        : E extends { $split: any } ? string[]
        : E extends { $cond: readonly [any, infer A, infer B] } ? DocExprType<T, A> | DocExprType<T, B>
        : E extends { $cond: { then: infer A; else: infer B } } ? DocExprType<T, A> | DocExprType<T, B>
        : E extends { $ifNull: readonly [infer A, ...infer R] } ? NonNullable<DocExprType<T, A>> | DocExprType<T, R[number]>
        : E extends { $switch: { branches: readonly { then: infer A }[]; default?: infer D } } ? DocExprType<T, A> | DocExprType<T, D>
        : E extends { $arrayElemAt: readonly [infer A, any] } | { $first: infer A } | { $last: infer A } ? DocElem<DocExprType<T, A>>
        : E extends { $slice: readonly [infer A, ...any[]] } | { $concatArrays: readonly [infer A, ...any[]] } ? DocExprType<T, A>
        : E extends { $min: infer A } | { $max: infer A } ? (A extends readonly unknown[] ? DocExprType<T, A[number]> : DocElem<DocExprType<T, A>> | Exclude<DocExprType<T, A>, readonly unknown[]>)
        : E extends { $map: any } | { $filter: any } | { $concatArrays: any } ? unknown[]
        : E extends { $objectToArray: any } ? { k: string; v: unknown }[]
        : E extends { $mergeObjects: any } ? Doc
        : E extends { readonly [k: string]: any } ? { -readonly [K in keyof E]: DocExprType<T, E[K]> }
        : unknown;

    /** $group accumulators. */
    type DocAccumulator<T> =
        | { $sum: DocExpr<T> } | { $avg: DocExpr<T> } | { $min: DocExpr<T> } | { $max: DocExpr<T> }
        | { $first: DocExpr<T> } | { $last: DocExpr<T> } | { $push: DocExpr<T> } | { $addToSet: DocExpr<T> } | { $count: {} };
    type DocAccType<T, A> =
        A extends { $sum: any } | { $avg: any } | { $count: any } ? number
        : A extends { $min: infer E } | { $max: infer E } | { $first: infer E } | { $last: infer E } ? DocExprType<T, E>
        : A extends { $push: infer E } | { $addToSet: infer E } ? DocExprType<T, E>[]
        : unknown;
    /** $project / $addFields value: 0|1|true|false keeps or drops; anything else is a computed expression. */
    type DocProjectSpec<T> = { readonly [field: string]: 0 | 1 | boolean | DocExpr<T> };
    /** $unwind argument: "$arrayPath" or the long form. */
    type DocUnwind<T> = `$${DocArrayPaths<T>}` | { path: `$${DocArrayPaths<T>}`; includeArrayIndex?: string; preserveNullAndEmptyArrays?: boolean };
    /** One aggregation stage over input documents T (stage set of dd-agg.c; $lookup is the equality form only). */
    type DocStage<T = Doc, S extends DocSchemas = DocAnySchemas> =
        | { $match: DocFilter<T> }
        | { $project: DocProjectSpec<T> }
        | { $addFields: DocProjectSpec<T> } | { $set: DocProjectSpec<T> }
        | { $unset: DocPaths<T> | readonly DocPaths<T>[] }
        | { $unwind: DocUnwind<T> }
        | { $group: { _id: DocExpr<T> } & { readonly [field: string]: DocAccumulator<T> | DocExpr<T> } }
        | { $bucket: { groupBy: DocExpr<T>; boundaries: readonly (number | string | Date)[]; default?: DocValue; output?: { readonly [field: string]: DocAccumulator<T> } } }
        | { $sortByCount: DocExpr<T> }
        | { $sort: DocSort<T> | { readonly [field: string]: 1 | -1 } }
        | { $skip: number } | { $limit: number } | { $count: string }
        | { $lookup: { from: keyof S & string; localField: DocPaths<T>; foreignField: string; as: string } }
        | { $facet: { readonly [output: string]: readonly DocStage<T, S>[] } }
        | { $replaceRoot: { newRoot: DocExpr<T> } } | { $replaceWith: DocExpr<T> };

    type DocUnwound<T, P extends string, A> = Omit<T, DocTop<P>> & {
        [K in DocTop<P>]: P extends `${string}.${string}` ? unknown : A extends { preserveNullAndEmptyArrays: true } ? DocElem<NonNullable<DocPathValue<T, P>>> | null | undefined : DocElem<NonNullable<DocPathValue<T, P>>>;
    } & (A extends { includeArrayIndex: infer I extends string } ? { [K in I]: number | null } : {});
    type DocProjectOut<T, Pr> = [{ [K in keyof Pr]: Pr[K] extends 1 | true ? K : never }[keyof Pr]] extends [never]
        ? [{ [K in keyof Pr]: Pr[K] extends 0 | false ? never : K }[keyof Pr]] extends [never]
            ? Omit<T, Extract<keyof Pr, keyof T>>
            : DocComputed<T, Pr> & ("_id" extends keyof T ? Pr extends { _id: 0 | false } ? {} : { _id: T["_id" & keyof T] } : {})
        : DocComputed<T, Pr> & ("_id" extends keyof T ? Pr extends { _id: 0 | false } ? {} : { _id: T["_id" & keyof T] } : {});
    type DocComputed<T, Pr> = { -readonly [K in keyof Pr as Pr[K] extends 0 | false ? never : K]: Pr[K] extends 1 | true ? (K extends DocPaths<T> ? DocPathValue<T, K & string> : unknown) : DocExprType<T, Pr[K]> };
    type DocGroupOut<T, G> = { -readonly [K in keyof G]: K extends "_id" ? DocExprType<T, G[K]> : DocAccType<T, G[K]> };
    /** The output document type of one stage applied to T. */
    type DocStageOut<T, St, S extends DocSchemas> =
        St extends { $match: any } | { $sort: any } | { $skip: any } | { $limit: any } ? T
        : St extends { $project: infer Pr } ? DocProjectOut<T, Pr>
        : St extends { $addFields: infer Pr } | { $set: infer Pr } ? Omit<T, keyof Pr> & DocComputed<T, Pr>
        : St extends { $unset: infer U } ? Omit<T, Extract<DocTop<U extends readonly (infer X)[] ? X : U>, string>>
        : St extends { $unwind: `$${infer P}` } ? DocUnwound<T, P, {}>
        : St extends { $unwind: { path: `$${infer P}` } & infer A } ? DocUnwound<T, P, A>
        : St extends { $group: infer G } ? DocGroupOut<T, G>
        : St extends { $bucket: { output: infer O } } ? { _id: unknown } & DocGroupOut<T, O>
        : St extends { $bucket: any } ? { _id: unknown; count: number }
        : St extends { $sortByCount: infer E } ? { _id: DocExprType<T, E>; count: number }
        : St extends { $count: infer N extends string } ? { [K in N]: number }
        : St extends { $lookup: { from: infer F; as: infer A extends string } } ? T & { [K in A]: DocWithId<F extends keyof S ? S[F] : Doc>[] }
        : St extends { $facet: infer F } ? { -readonly [K in keyof F]: DocPipelineOut<T, F[K], S>[] }
        : St extends { $replaceRoot: { newRoot: infer E } } | { $replaceWith: infer E } ? DocExprType<T, E>
        : Doc;
    /** The output document type of a whole pipeline (folded stage by stage); a non-literal Stage[] yields Doc. */
    type DocPipelineOut<T, P, S extends DocSchemas = DocAnySchemas> =
        P extends readonly [] ? T
        : P extends readonly [infer St, ...infer R] ? DocPipelineOut<DocStageOut<T, St, S>, R, S>
        : Doc;

    /** A schema map for a Database: collection name -> document type (types $lookup and db.collection(name)). */
    type DocSchemas = object;
    type DocAnySchemas = { [collection: string]: Doc };

    /** A change event: insert/replace carry fullDocument, update carries updateDescription (and fullDocument under
     *  updateLookup, null once deleted), delete only documentKey, drop ns only, invalidate neither; _id is the resume token. */
    type ChangeEvent<T = Doc> = { _id: { _data: string }; wallTime: Date } & (
        | { operationType: "insert" | "replace"; ns: { db: string; coll: string }; documentKey: { _id: DocWithId<T>["_id"] }; fullDocument: DocWithId<T> }
        | { operationType: "update"; ns: { db: string; coll: string }; documentKey: { _id: DocWithId<T>["_id"] }; updateDescription: { updatedFields: { [path: string]: DocValue }; removedFields: string[] }; fullDocument?: DocWithId<T> | null }
        | { operationType: "delete"; ns: { db: string; coll: string }; documentKey: { _id: DocWithId<T>["_id"] } }
        | { operationType: "drop"; ns: { db: string; coll: string } }
        | { operationType: "invalidate" });
    /** watch() options; resumeAfter/startAfter take an event _id or resumeToken and deliver every later event. */
    interface WatchOptions {
        fullDocument?: "default" | "updateLookup";
        resumeAfter?: { _data: string };
        startAfter?: { _data: string };
    }
    /** watch() pipeline stages ($match/$project/$addFields/$set/$unset/$replaceRoot/$replaceWith; others BadValue 2). */
    type DocWatchStage<T = Doc> = Extract<DocStage<ChangeEvent<T>>, { $match: any } | { $project: any } | { $addFields: any } | { $set: any } | { $unset: any } | { $replaceRoot: any } | { $replaceWith: any }>;
    /** An open change stream over a bounded per-collection ring (changeStreamCapacity, default 4096, memory only);
     *  a reader that falls out of the ring, or a token from before recording/a drop/another Database, is HistoryLost 286. */
    class ChangeStream<E = ChangeEvent> implements AsyncIterable<E> {
        /** Streams come from watch(); calling this throws. */
        constructor();
        /** The next event, waiting for a write; { done: true } once closed or invalidated; pending calls resolve in order. */
        next(): Promise<IteratorResult<E>>;
        /** The next ready event or null; throws on HistoryLost; refused while a next() waits. */
        tryNext(): E | null;
        /** Ends the stream; waiting next() calls resolve done. Idempotent. A pending next() blocks Database.close(). */
        close(): void;
        return(): Promise<IteratorResult<E>>;
        /** Token of the last event read (or the start point), for watch([], { resumeAfter }). */
        readonly resumeToken: { _data: string };
        readonly closed: boolean;
        [Symbol.asyncIterator](): ChangeStream<E>;
    }

    /** Snapshot result. File: header {"DYNADOC\0", v1, created ms, sections, CRC32C} + sections {kind, length, CRC32C}
     *  + footer {"DYNAEND\0", length, count, CRC32C of CRCs}; indexes are rebuilt on load; any damage is 100001. */
    interface PersistResult { path: string; bytes: number; collections: number; documents: number }

    /** A collection of T documents; created by its first write or createCollection. Reads of a missing collection, and
     *  writes that change nothing, answer as empty and do not create it (dropIndex there is NamespaceNotFound 26). */
    class Collection<T extends object = Doc, S extends DocSchemas = DocAnySchemas> {
        /** Collections come from db.collection(name); calling this throws. */
        constructor();
        readonly name: string;
        /** Adds an ObjectId _id when absent (the argument is not modified). */
        insertOne(doc: DocInsert<T>): WriteResult & { insertedId: DocWithId<T>["_id"] };
        /** ordered (default true) stops at the first failure; any failure throws DocDBError with writeErrors after applying the rest. */
        insertMany(docs: readonly DocInsert<T>[], opts?: { ordered?: boolean }): WriteResult & { insertedCount: number; insertedIds: Record<number, DocWithId<T>["_id"]> };
        /** Matches in sort order (else slot order) after skip/limit; a sort+limit on an indexed field walks the index and stops early. */
        find<const Pr extends DocProjection<T> = {}>(filter?: DocFilter<T>, opts?: FindOptions<T, Pr>): DocProjected<T, Pr>[];
        findOne<const Pr extends DocProjection<T> = {}>(filter?: DocFilter<T>, opts?: FindOptions<T, Pr>): DocProjected<T, Pr> | null;
        /** Matches after skip/limit; one $eq/$in/range on a single-field non-multikey index counts postings without reading documents. */
        countDocuments(filter?: DocFilter<T>, opts?: { skip?: number; limit?: number }): number;
        estimatedDocumentCount(): number;
        /** Distinct values of a dotted field among the matches, array elements individually, first-seen order. */
        distinct<const P extends DocPaths<DocWithId<T>>>(field: P, filter?: DocFilter<T>): (DocElem<DocPathValue<DocWithId<T>, P>> | Exclude<DocPathValue<DocWithId<T>, P>, readonly unknown[]>)[];
        /** Updates the first match (in sort order); upsert inserts the filter's equality fields with the update applied. */
        updateOne(filter: DocFilter<T>, update: DocUpdate<T>, opts?: { upsert?: boolean; sort?: DocSort<T> }): UpdateResult<T>;
        /** Updates every match; a unique-index violation stops there with earlier documents applied. */
        updateMany(filter: DocFilter<T>, update: DocUpdate<T>, opts?: { upsert?: boolean }): UpdateResult<T>;
        /** Replaces the first match keeping its _id (a different _id is ImmutableField 66). */
        replaceOne(filter: DocFilter<T>, replacement: DocInsert<T>, opts?: { upsert?: boolean }): UpdateResult<T>;
        deleteOne(filter: DocFilter<T>): WriteResult & { deletedCount: number };
        deleteMany(filter: DocFilter<T>): WriteResult & { deletedCount: number };
        /** Updates the first match in sort order; returns it before (default) or after, or null. */
        findOneAndUpdate<const Pr extends DocProjection<T> = {}>(filter: DocFilter<T>, update: DocUpdate<T>, opts?: FindOptions<T, Pr> & { upsert?: boolean; returnDocument?: "before" | "after" }): DocProjected<T, Pr> | null;
        findOneAndReplace<const Pr extends DocProjection<T> = {}>(filter: DocFilter<T>, replacement: DocInsert<T>, opts?: FindOptions<T, Pr> & { upsert?: boolean; returnDocument?: "before" | "after" }): DocProjected<T, Pr> | null;
        /** Deletes the first match in sort order and returns it, or null. */
        findOneAndDelete<const Pr extends DocProjection<T> = {}>(filter: DocFilter<T>, opts?: FindOptions<T, Pr>): DocProjected<T, Pr> | null;
        /** ordered (default true) stops at the first failure. */
        bulkWrite(ops: readonly DocBulkOp<T>[], opts?: { ordered?: boolean }): BulkWriteResult<T>;
        /** Builds an index (up to 32 fields) and returns its name (default "a_1_b_-1"); same spec again is a no-op, same name other spec 85/86. */
        createIndex(keys: DocSort<T>, opts?: { name?: string; unique?: boolean; sparse?: boolean; partialFilterExpression?: DocFilter<T> }): string;
        /** By name or key pattern; the implicit _id_ index cannot be dropped. */
        dropIndex(nameOrKeys: string | DocSort<T>): void;
        listIndexes(): DocIndexSpec<T>[];
        /** Plans and runs the query; reports the winning and rejected plans and execution stats. */
        explain(filter?: DocFilter<T>, opts?: FindOptions<T>): DocExplain;
        /** Runs a pipeline; output type is inferred stage by stage. A leading $match/$sort/$skip/$limit uses indexes; inputs of
         *  2048+ documents run on the pool unless a $match uses $regex. Every failure rejects (40324 stage, 168 operator, 16608 /0). */
        aggregate<const P extends readonly DocStage<DocWithId<T>, S>[]>(pipeline: P): Promise<DocPipelineOut<DocWithId<T>, P, S>[]>;
        /** A change stream over this collection (need not exist yet), from now or after a resume token. */
        watch(pipeline?: readonly DocWatchStage<T>[], opts?: WatchOptions): ChangeStream<ChangeEvent<T>>;
        /** Snapshot of this collection alone. */
        persist(path: string, opts?: { fsync?: boolean }): Promise<PersistResult>;
        /** Drops the collection and its indexes; false when it did not exist. */
        drop(): boolean;
    }
    /** An in-memory database; S maps collection names to document types. close() is refused while persist/load runs. */
    class Database<S extends DocSchemas = DocAnySchemas> implements DynResource {
        /** name defaults to "test"; changeStreamCapacity bounds each collection's event ring (default 4096). */
        constructor(name?: string, opts?: { changeStreamCapacity?: number });
        /** Reads a snapshot into a new Database (name restored); any damage rejects InvalidSnapshotFile 100001. */
        static load<S extends DocSchemas = DocAnySchemas>(path: string): Promise<Database<S>>;
        readonly name: string;
        /** A handle on a collection, which need not exist yet; typed by S, or explicitly with collection<T>(name). */
        collection<N extends keyof S & string>(name: N): Collection<S[N] & object, S>;
        collection<T extends object = Doc>(name: string): Collection<T, S>;
        /** Creates an empty collection (NamespaceExists 48 when present); options are persisted. */
        createCollection<N extends keyof S & string>(name: N, opts?: { changeStreamCapacity?: number }): Collection<S[N] & object, S>;
        createCollection<T extends object = Doc>(name: string, opts?: { changeStreamCapacity?: number }): Collection<T, S>;
        listCollections(): (keyof S & string)[];
        dropCollection(name: keyof S & string | string): boolean;
        /** Every collection to one snapshot file off the JS thread; later writes are not in it. */
        persist(path: string, opts?: { fsync?: boolean }): Promise<PersistResult>;
        /** Replaces the database with a snapshot (merge: true replaces only the collections the file holds). */
        load(path: string, opts?: { merge?: boolean }): Promise<PersistResult>;
        /** A stream over every collection in write order; a drop is a "drop" event and does not end it. */
        watch(pipeline?: readonly DocWatchStage<S[keyof S]>[], opts?: WatchOptions): ChangeStream<ChangeEvent<S[keyof S] & object>>;
        stats(): { collections: number; documents: number; dataBytes: number; indexes: number; indexEntries: number; retiredBytes: number; pinnedSnapshots: number };
        close(): void;
        dispose(): void;
        readonly closed: boolean;
        readonly [Symbol.dispose]: () => void;
    }
}
/** std — QuickJS-compatible stdio and environment module, present only under --std; failures are errno values, not throws. */
declare module "std" {
    interface StdFile {
        /** Method, not a property (matches the implementation). */
        eof(): boolean;
        /** Method, not a property. */
        error(): boolean;
        getByte(): number;
        putByte(b: number): void;
        readBytes(max: number): Uint8Array;
        readAsString(max: number): string;
        writeBytes(bytes: Uint8Array): void;
        writeStr(str: string): void;
        getline(): string | null;
        fileno(): number;
        close(): void;
        seek(offset: number, whence: number): void;
        tell(): number;
    }
    const __stdin: StdFile;
    const __stdout: StdFile;
    const __stderr: StdFile;
    export { __stdin as in, __stdout as out, __stderr as err };
    /** Format and print to stdout. Width/precision (digits and `*`) capped at 1000000 (RangeError); %c honours width/flags like C. */
    function printf(fmt: string, ...args: unknown[]): void;
    /** Format to a string. Width/precision (digits and `*`) capped at 1000000 (RangeError); %c honours width/flags like C. */
    function sprintf(fmt: string, ...args: unknown[]): string;
    function puts(str: string): void;
    function getenv(name: string): string | undefined;
    function setenv(name: string, value: string): void;
    function unsetenv(name: string): void;
    /** The whole environment as a {name: value} record. */
    function getenviron(): Record<string, string>;
    /** Exit now via exit(3): flushes the dyna:stream stdout()/stderr() buffers first; an unreported write error found by that flush is printed and turns code 0 into 1. */
    function exit(code?: number): void;
    function gc(): void;
    /** Evaluates the given script in global scope. */
    function evalScript(script: string, options?: unknown): unknown;
    /** Loads a script file and evaluates it. */
    function loadScript(filename: string): unknown;
    /** Reads a whole file as a string, or null when it cannot be read. */
    function loadFile(filename: string): string | null;
    /** Opens a file; mode "r", "w", "a", "r+", ... */
    function open(filename: string, mode: string, error?: Error): StdFile | null;
    function fdopen(fd: number, mode: string, error?: Error): StdFile | null;
    function tmpfile(): StdFile;
    /** strerror(errno). */
    function strerror(errno: number): string;
    /** JSON parse with extensions (Date serialization, ...). */
    function parseExtJSON(str: string): unknown;
    /** Internal: pretty-prints a value for print(). */
    function __printObject(obj: unknown): void;
    /** The standard Error constructor. */
    const Error: ErrorConstructor;
    const SEEK_SET: number;
    const SEEK_CUR: number;
    const SEEK_END: number;
}

/** os — QuickJS-compatible POSIX layer (fds, processes, signals, timers), only under --std; calls return a negative errno instead of throwing.
 *  os.Worker is the parallelism primitive: a separate runtime with its own heap, exchanging structured-clone messages. */
declare module "os" {
    /** The number of milliseconds since an arbitrary point, microsecond resolution; the same clock as performance.now() and every timer deadline (macOS: mach_continuous_time, which keeps counting across sleep like CLOCK_MONOTONIC; elsewhere CLOCK_MONOTONIC). Each arming (setTimeout, setInterval re-arm, sleep) reads it fresh, so no timer fires before its delay as measured by this clock; a timer already due by the last reading fires without reading it again when no I/O, worker port or reactor is registered. */
    function now(): number;
    /** The OS name; a string property, not a function. */
    const platform: string;
    function getpid(): number;
    /** [cwd, errorcode]. */
    function getcwd(): [string, number];
    /** 0 or a negative errno. */
    function chdir(path: string): number;
    interface OsStat {
        dev: number;
        ino: number;
        mode: number;
        nlink: number;
        uid: number;
        gid: number;
        rdev: number;
        size: number;
        blocks: number;
        atime: number;
        mtime: number;
        ctime: number;
    }
    /** [stat, errorcode]; times are milliseconds since epoch. */
    function stat(path: string): [OsStat, number];
    /** [stat, errorcode], not following symlinks; times in ms. */
    function lstat(path: string): [OsStat, number];
    function readdir(path: string): [string[], number];
    function readlink(path: string): [string, number];
    function realpath(path: string): [string, number];
    function rename(oldpath: string, newpath: string): number;
    /** A File backed by the regular file at path, read lazily: nothing is read or mapped at open, each read (slice bytes, text(), arrayBuffer(), stream() 64 KiB chunks, FileReader) preads only the bytes it needs from the path of the descriptor opened at open (F_GETPATH or /proc/self/fd, else realpath), so streaming a large file keeps memory near one chunk. name = the basename, lastModified = mtime, type from options (normalized like Blob type).
     *  File API snapshot: the device, inode, size and mtime are recorded at open; a read after any of them changed (rewrite, append, truncate, replace) fails with a NotReadableError DOMException (text()/arrayBuffer()/bytes() reject, stream() errors, FileReader fires error) instead of returning the new bytes. Each read stats the path (the identity check); the read-only fd opened to take the snapshot stays open as the File's read fd, so open + first read cost one open() (at most 64 such fds across all Files; beyond that the snapshot fd closes and each read opens and closes its own), released when the File is collected; reads of 4 KiB or less within one aligned 64 KiB block go through a single per-thread 64 KiB block shared by all Files (freed when the last File is collected), so a live File costs no block of its own. TypeError for a missing file or a non-regular file. */
    function openAsBlob(path: string, options?: { type?: string }): File;
    function remove(path: string): number;
    function mkdir(path: string, mode?: number): number;
    function symlink(target: string, linkpath: string): number;
    function utimes(path: string, atime: number, mtime: number): number;
    /** fd, or a negative errno. */
    function open(path: string, flags: number, mode?: number): number;
    function close(fd: number): number;
    /** Byte count read, or a negative errno; `buffer` is a raw ArrayBuffer sliced by offset/length. */
    function read(fd: number, buffer: ArrayBuffer, offset: number, length: number, position?: number): number;
    /** Byte count written, or a negative errno; `buffer` is a raw ArrayBuffer sliced by offset/length. */
    function write(fd: number, buffer: ArrayBuffer, offset?: number, length?: number, position?: number): number;
    /** New offset, or a negative errno. */
    function seek(fd: number, position: number, whence: number): number;
    /** New fd, or a negative errno. */
    function dup(fd: number): number;
    function dup2(oldfd: number, newfd: number): number;
    /** [readFd, writeFd], or null. */
    function pipe(): [number, number] | null;
    function isatty(fd: number): boolean;
    function ttySetRaw(fd: number, raw: boolean): void;
    function ttyGetWinSize(fd: number): [number, number] | null;
    /** Runs a process and returns its exit status (negative = killed by that signal); block:false returns the pid. Legacy form: dyna:sys's Exec is canonical. */
    function exec(args: string[], options?: { blocking?: boolean; usePath?: boolean; file?: string; cwd?: string; stdin?: unknown; stdout?: unknown; stderr?: unknown; env?: Record<string, string>; uid?: number; gid?: number }): number;
    function waitpid(pid: number, options?: number): [number, number];
    function kill(pid: number, sig: number): number;
    /** The handler is invoked with no arguments; read the registered number from closure state. */
    function signal(signal: number, handler: () => void): void;
    function sleep(delay: number): void;
    function setTimeout(cb: (...args: unknown[]) => void, delay: number, ...args: unknown[]): number;
    function clearTimeout(id: number): void;
    function setInterval(cb: (...args: unknown[]) => void, delay: number, ...args: unknown[]): number;
    function clearInterval(id: number): void;
    /** Counters of THIS thread's event loop since start: blocking waits entered, timer callbacks, fd-handler callbacks, Worker messages delivered, cross-thread completions run, signal handlers run; `watched` is how many fd handlers sit in the kqueue/epoll set (0 below 16 handlers, where plain poll() is cheaper). `ioWaits` counts this thread's native I/O reactor calls into the kernel readiness API (kevent/epoll_wait/epoll_ctl/poll) and `ioChanges` the interest changes they carried: on kqueue, fd interest changes are queued and submitted with the next wait in one kevent call (one syscall per loop turn), so a change undone within the same turn costs nothing. On io_uring builds `ioWaits` and `ioChanges` are always 0: the io_uring reactor does not count its submissions. An idle process must not grow `waits`. */
    function loopStats(): { waits: number; timers: number; fdEvents: number; messages: number; posts: number; signals: number; watched: number; ioWaits: number; ioChanges: number };
    function setReadHandler(fd: number, cb: (fd: number) => void): void;
    function setWriteHandler(fd: number, cb: (fd: number) => void): void;
    const O_RDONLY: number;
    const O_WRONLY: number;
    const O_RDWR: number;
    const O_APPEND: number;
    const O_CREAT: number;
    const O_EXCL: number;
    const O_TRUNC: number;
    /** Windows only; absent on POSIX builds. */
    const O_BINARY: number | undefined;
    /** Windows only; absent on POSIX builds. */
    const O_TEXT: number | undefined;
    const WNOHANG: number;
    const SIGABRT: number;
    const SIGALRM: number;
    const SIGCHLD: number;
    const SIGCONT: number;
    const SIGFPE: number;
    const SIGILL: number;
    const SIGINT: number;
    const SIGPIPE: number;
    const SIGQUIT: number;
    const SIGSEGV: number;
    const SIGSTOP: number;
    const SIGTERM: number;
    const SIGTSTP: number;
    const SIGTTIN: number;
    const SIGTTOU: number;
    const SIGUSR1: number;
    const SIGUSR2: number;
    const S_IFMT: number;
    const S_IFBLK: number;
    const S_IFCHR: number;
    const S_IFDIR: number;
    const S_IFIFO: number;
    const S_IFLNK: number;
    const S_IFREG: number;
    const S_IFSOCK: number;
    const S_ISUID: number;
    const S_ISGID: number;
    const Worker: {
        new (script: string, options?: unknown): Worker;
        /** Inside a worker script: the port back to the parent; null on the main thread. */
        readonly parent: Worker | null;
    };
    interface Worker {
        /** Optional transfer list: buffers are copied to the receiver and detached on the sender; a SharedArrayBuffer is refused. An array is written index by index with holes as undefined, so one with more than 2^24 holes beyond its real elements throws RangeError.
         *  ReadableStream, WritableStream and TransformStream are transferable: the sender's stream is locked and piped over a private native port pair, and the receiver gets a new stream
         *  wherever the original sat in `value` (in its arrays and objects; an object holding one is copied as a plain object). Map and Set are not structured-cloneable here at all (TypeError "unsupported object class", with or without streams in them), so a stream inside one is refused like any other unclonable value. A stream in `value` but not in the list is a TypeError ("unsupported object class"), as are a locked stream and one listed twice;
         *  a top-level "@@dyna.streams.envelope" key marks a message that carries streams (do not use it in your own messages), and "@@dyna.streams.ref" objects stand for the streams inside one.
         *  Nothing is locked if the post fails. Chunks are structured-cloned one per pull (pull-driven, as the spec's cross-realm transform); an Error reason crosses as its class and message.
         *  A receiver that exits without reading makes the sender's side error with "the other side of a transferred stream went away". */
        postMessage(value: unknown, transfer?: (ArrayBuffer | ArrayBufferView | ReadableStream | WritableStream | TransformStream)[]): void;
        /** A live message port keeps the process alive even after the worker exits; set to null to release it. */
        onmessage: ((ev: { data: unknown }) => void) | null;
        onerror: ((err: unknown) => void) | null;
    }
}

/** dyna:bench — a micro-benchmark harness: bench() times a function in adaptive batches on the monotonic clock, table() prints the results. */
declare module "dyna:bench" {
    /** One result: opsPerSec = 1000/meanMs, rsd = std/mean. Samples are batches doubled until each lasts 0.1 ms, so tiny bodies are not measured against the clock tick. */
    interface BenchResult {
        name: string;
        iters: number;
        elapsedMs: number;
        opsPerSec: number;
        rsd: number;
        p50Ms: number;
        p99Ms: number;
        meanMs: number;
    }
    /** Runs fn for timeMs (default 500) after warmupMs (default 100) and records the result for table(). */
    function bench(name: string, fn: () => void, opts?: { timeMs?: number; warmupMs?: number; warmup?: number }): BenchResult;
    /** Tab-separated table of all bench() results this process (header + a row per run), retained for the process lifetime; no reset API. */
    function table(): string;
}

// ---- WHATWG globals (Fetch, DOM AbortSignal, XHR FormData). Oracle: WPT via tools/wpt.js (fetch/api/{headers,request,response,body}, xhr/formdata) with tests/wpt-expectations.tsv; AbortSignal by tests/net/test_fetch_abort_signal_spec.js.
// Spec first (owner rule 12). Known omissions (missing surface, not deviations): Event/EventTarget, Response/Request textStream (WPT body/textstream, a proposal). AbortSignal default reasons are DOMException AbortError/TimeoutError.
// Deviations kept on purpose (server runtime, matching Deno/undici): no browser header guards (forbidden request/response header names, no-cors safelist filtering, Set-Cookie on Response headers is allowed);
// the init dictionaries are STRICT (an unknown key throws TypeError naming the valid set; WebIDL would ignore it); fetch() adds timeout/onConnect/cookieJar; ResponseInit adds url; a FormData value may also be a byte buffer
// (Uint8Array/ArrayBuffer, sent as a file part, the filename reaches the part); redirect "manual" returns the 3xx response itself (type "basic", not "opaqueredirect"); a cross-origin redirect strips Authorization, Cookie and Proxy-Authorization (the spec strips Authorization only).

/** Identity for `using` disposal. */
interface SymbolConstructor {
    readonly dispose: symbol;
    readonly asyncDispose: symbol;
}

/** WHATWG AbortSignal (DOM "Interface AbortSignal"). Not constructible (TypeError); get one from AbortController, AbortSignal.abort/timeout/any or Request.signal.
 *  Listeners: only "abort" is stored; a listener is a function or an object with handleEvent; duplicates (same callback and capture) are ignored; options once, capture, passive and signal (an aborted options.signal removes it) are honoured; a listener added after the abort is never called.
 *  Abort order (spec "signal abort"): reason set, dependents marked aborted, this signal's listeners (onabort in its first-assignment position), then each dependent's listeners. A listener that throws is reported (rethrown in a microtask) and dispatch continues.
 *  Default reasons are DOMException instances: AbortError "signal is aborted without reason" (abort) and TimeoutError "signal timed out" (timeout). The event is a plain { type: "abort", target, currentTarget } object (no Event class). */
interface AbortSignal {
    readonly aborted: boolean;
    readonly reason: unknown;
    onabort: ((this: AbortSignal, ev: { type: string; target: AbortSignal; currentTarget: AbortSignal }) => void) | null;
    addEventListener(type: "abort", listener: ((ev: { type: string; target: AbortSignal; currentTarget: AbortSignal }) => void) | { handleEvent(ev: { type: string; target: AbortSignal }): void } | null, options?: boolean | { capture?: boolean; once?: boolean; passive?: boolean; signal?: AbortSignal }): void;
    removeEventListener(type: "abort", listener: ((ev: { type: string; target: AbortSignal }) => void) | { handleEvent(ev: { type: string; target: AbortSignal }): void } | null, options?: boolean | { capture?: boolean }): void;
    throwIfAborted(): void;
}
interface AbortSignalConstructor {
    abort(reason?: unknown): AbortSignal;
    /** Aborts with a TimeoutError after `delayMs` (a negative or non-finite delay is a TypeError). fetch() clears a timeout signal's timer when the fetch settles. */
    timeout(delayMs: number): AbortSignal;
    /** A dependent signal: aborted now if any source already is (with the first such reason), else when the first source aborts. Every element must be an AbortSignal (TypeError). */
    any(signals: Iterable<AbortSignal>): AbortSignal;
}
declare const AbortSignal: AbortSignalConstructor;

/** WHATWG AbortController: abort() fires its `signal`. */
interface AbortController {
    readonly signal: AbortSignal;
    abort(reason?: unknown): void;
}
interface AbortControllerConstructor {
    new (): AbortController;
}
declare const AbortController: AbortControllerConstructor;

type HeadersInit = Headers | [string, string][] | Iterable<Iterable<string>> | Record<string, string>;
/** WHATWG Headers. Names must be HTTP tokens and values may not contain NUL/CR/LF or code points above U+00FF (TypeError); values are trimmed of HTTP whitespace (tab, LF, CR, space).
 *  A repeated name combines with ", " (get() returns the combined value) except set-cookie, whose values stay separate (getSetCookie(), and one iteration entry each).
 *  Iteration (keys/values/entries/forEach/for-of) is the spec's "sort and combine": names lower-cased and sorted, and it is live: each next() re-reads the current list at the iterator's index.
 *  init: a sequence of [name, value] pairs (each exactly 2 items, else TypeError) or a record (own enumerable string keys; a Symbol key is a TypeError), all converted before any is added.
 *  Headers of a fetched response, of Response.error() and of Response.redirect() are immutable (TypeError on append/set/delete). */
interface Headers {
    append(name: string, value: string): void;
    delete(name: string): void;
    get(name: string): string | null;
    getSetCookie(): string[];
    has(name: string): boolean;
    set(name: string, value: string): void;
    forEach(cb: (value: string, key: string, parent: Headers) => void, thisArg?: unknown): void;
    keys(): IterableIterator<string>;
    values(): IterableIterator<string>;
    entries(): IterableIterator<[string, string]>;
    [Symbol.iterator](): IterableIterator<[string, string]>;
}
interface HeadersConstructor {
    new (init?: HeadersInit): Headers;
}
declare const Headers: HeadersConstructor;

/** WHATWG FormData (XHR spec): an ordered list of [name, value] entries. Names and string values are USVStrings (lone surrogates become U+FFFD); a non-buffer value is String()-converted (undefined -> "undefined").
 *  Iteration is live (each next() reads the current list at the iterator's index). Extension: a value may be a Uint8Array or ArrayBuffer, sent as a file part; a filename is allowed only with a Blob or such a value (a string value with a filename is a TypeError, as WebIDL overload resolution picks the Blob overload for three arguments). */
interface FormData {
    /** A Blob value is stored as a File (named filename, or "blob" / the File name when omitted) and sent as a file part with its type (else application/octet-stream); Uint8Array/ArrayBuffer stay bytes; anything else becomes a USVString. */
    append(name: string, value: string | Uint8Array | ArrayBuffer | Blob, filename?: string): void;
    delete(name: string): void;
    get(name: string): string | File | Uint8Array | ArrayBuffer | null;
    getAll(name: string): (string | File | Uint8Array | ArrayBuffer)[];
    has(name: string): boolean;
    set(name: string, value: string | Uint8Array | ArrayBuffer | Blob, filename?: string): void;
    forEach(cb: (value: string | File | Uint8Array | ArrayBuffer, key: string, parent: FormData) => void, thisArg?: unknown): void;
    keys(): IterableIterator<string>;
    values(): IterableIterator<string | File | Uint8Array | ArrayBuffer>;
    entries(): IterableIterator<[string, string | File | Uint8Array | ArrayBuffer]>;
    [Symbol.iterator](): IterableIterator<[string, string | File | Uint8Array | ArrayBuffer]>;
}
interface FormDataConstructor {
    /** Takes no form: any argument other than undefined is a TypeError (there is no HTMLFormElement). */
    new (): FormData;
}
declare const FormData: FormDataConstructor;

/** Global WebSocket: thin alias over dyna:http WsClient (same constructor). Present only with the "http" module; without the optional "tls" module a wss:// URL throws a TypeError naming "tls". */
declare const WebSocket: typeof import("dyna:http").WsClient;

/** Global WebRTC classes: the same objects dyna:webrtc exports, installed on first use (TLS builds only). */
declare const RTCPeerConnection: typeof import("dyna:webrtc").RTCPeerConnection;
declare const RTCDataChannel: typeof import("dyna:webrtc").RTCDataChannel;
declare const RTCIceCandidate: typeof import("dyna:webrtc").RTCIceCandidate;
declare const RTCSessionDescription: typeof import("dyna:webrtc").RTCSessionDescription;
declare const RTCCertificate: typeof import("dyna:webrtc").RTCCertificate;
declare const RTCRtpTransceiver: typeof import("dyna:webrtc").RTCRtpTransceiver;
declare const RTCRtpSender: typeof import("dyna:webrtc").RTCRtpSender;
declare const RTCRtpReceiver: typeof import("dyna:webrtc").RTCRtpReceiver;
declare const MediaStream: typeof import("dyna:webrtc").MediaStream;
declare const MediaStreamTrack: typeof import("dyna:webrtc").MediaStreamTrack;

/** Options for fetch() and new Request() (WebIDL RequestInit; members are read in lexicographic order; enum members are validated with a TypeError). */
interface RequestInit {
    /** Extracted at construction (Fetch "extract a body"): string -> UTF-8 bytes with content-type "text/plain;charset=UTF-8"; ArrayBuffer/any ArrayBufferView -> a copy of its bytes; a Blob -> its bytes, with its type as the content-type when non-empty; URLSearchParams -> urlencoded bytes with
     *  "application/x-www-form-urlencoded;charset=UTF-8"; FormData -> the multipart bytes, serialized now, with "multipart/form-data; boundary=..." (later changes to the FormData do not affect the body);
     *  a ReadableStream (requires duplex: "half"; TypeError if locked or disturbed; fetch() reads it whole before sending); anything else -> String(body). The content-type is added only when headers lack one.
     *  A GET or HEAD request with a body (given or inherited from an input Request) is a TypeError. */
    body?: unknown;
    cache?: "default" | "no-store" | "reload" | "no-cache" | "force-cache" | "only-if-cached";
    credentials?: "omit" | "same-origin" | "include";
    /** Only "half"; required when body is a ReadableStream. */
    duplex?: "half";
    headers?: HeadersInit;
    integrity?: string;
    keepalive?: boolean;
    /** An HTTP token (TypeError otherwise); CONNECT, TRACE and TRACK are forbidden (TypeError); DELETE, GET, HEAD, OPTIONS, POST and PUT are upper-cased case-insensitively, other methods keep their case. */
    method?: string;
    /** "navigate" is a TypeError. "no-cors" allows only GET, HEAD and POST. */
    mode?: "cors" | "same-origin" | "no-cors" | "navigate";
    priority?: "high" | "low" | "auto";
    /** "follow" (default), "manual" (return the 3xx itself) or "error" (reject on a redirect). */
    redirect?: "follow" | "manual" | "error";
    /** "" means no referrer; otherwise a URL (TypeError if it does not parse). Recorded only: fetch() sends no Referer header from it. */
    referrer?: string;
    referrerPolicy?: "" | "no-referrer" | "no-referrer-when-downgrade" | "same-origin" | "origin" | "strict-origin" | "origin-when-cross-origin" | "strict-origin-when-cross-origin" | "unsafe-url";
    signal?: AbortSignal | null;
    /** Must be null or undefined (TypeError otherwise). */
    window?: null;
    /** Milliseconds until the fetch fails; bounds the WHOLE exchange (connect + body). fetch() only. */
    timeout?: number;
    /** Connect hook as HTTPClient.onConnect: resolved IP; return false or throw to refuse. fetch() only. */
    onConnect?: (ip: string) => unknown;
    /** A dyna:http CookieJar: sent and filled on every hop (fetch() only; an explicit Cookie header wins over it). */
    cookieJar?: import("dyna:http").CookieJar;
    /** dyna meta configuration (fetch() only): see HTTPClientMetaOptions; held for every redirect hop. An invalid bag rejects (TypeError or RangeError). */
    dyna?: import("dyna:http").HTTPClientMetaOptions;
}
/** WHATWG Request. The URL is parsed with URL (relative URLs resolve against globalThis.location.href when it exists); an invalid URL or one with a username/password is a TypeError.
 *  new Request(request, init) copies the input; when the body comes from the input Request, the input becomes used (bodyUsed true) and must not already be unusable (TypeError). */
interface Request {
    readonly method: string;
    readonly url: string;
    readonly headers: Headers;
    readonly destination: "";
    /** "about:client" by default, "" for no referrer, else the parsed URL. */
    readonly referrer: string;
    readonly referrerPolicy: string;
    readonly mode: "cors" | "same-origin" | "no-cors";
    readonly credentials: "omit" | "same-origin" | "include";
    readonly cache: string;
    readonly redirect: "follow" | "manual" | "error";
    readonly integrity: string;
    readonly keepalive: boolean;
    readonly isReloadNavigation: false;
    readonly isHistoryNavigation: false;
    /** Always an AbortSignal: a new signal that follows init.signal (or the input Request's signal), created at first access. */
    readonly signal: AbortSignal;
    readonly duplex: "half";
    /** The body as a byte ReadableStream, or null. */
    readonly body: ReadableStream<Uint8Array> | null;
    readonly bodyUsed: boolean;
    /** TypeError when the body is unusable; a stream body is tee()d. */
    clone(): Request;
    arrayBuffer(): Promise<ArrayBuffer>;
    bytes(): Promise<Uint8Array>;
    json(): Promise<unknown>;
    text(): Promise<string>;
    /** The body bytes as a Blob whose type is the Content-Type header ("" when absent). */
    blob(): Promise<Blob>;
    /** Fetch formData(): multipart/form-data (fields UTF-8 decoded without BOM; file parts become Files typed by their Content-Type, else text/plain) or application/x-www-form-urlencoded (bytes UTF-8 decoded whatever the charset; a leading "?" is kept). Any other MIME type, a missing boundary or a malformed body rejects with TypeError. */
    formData(): Promise<FormData>;
}
interface RequestConstructor {
    new (input: string | URL | Request, init?: RequestInit): Request;
}
declare const Request: RequestConstructor;

/** Options for new Response() and Response.json(). */
interface ResponseInit {
    /** WebIDL unsigned short, then 200..599 or RangeError. */
    status?: number;
    /** Default "" (Fetch spec). Must be a reason-phrase (tab, space, visible ASCII or U+0080..U+00FF) or TypeError. */
    statusText?: string;
    headers?: HeadersInit;
    /** Extension: the value of Response.url. */
    url?: string;
}
/** WHATWG Response. A body with a null-body status (101, 103, 204, 205, 304) is a TypeError. Body readers on a null body resolve as empty ("" / empty bytes; json() rejects with SyntaxError) and never mark it used. */
interface Response {
    /** "default" (constructed), "basic" (from fetch()), "error" (Response.error()). */
    readonly type: "default" | "basic" | "error" | "opaqueredirect";
    readonly url: string;
    /** True when fetch() followed at least one redirect. */
    readonly redirected: boolean;
    readonly status: number;
    readonly ok: boolean;
    readonly statusText: string;
    readonly headers: Headers;
    /** The body as a WHATWG byte ReadableStream (null for a null body), created at first access and the same object after. Once body exists, text()/json()/bytes()/arrayBuffer()
     *  read through it: they reject (TypeError) if it is locked or was read from, and a stream body's chunks must be Uint8Array. fetch() buffers the whole response first, so this streams the buffered bytes. */
    readonly body: ReadableStream<Uint8Array> | null;
    /** True after a body method ran or once the body stream was read from or canceled. */
    readonly bodyUsed: boolean;
    /** The body bytes as a Blob whose type is the Content-Type header ("" when absent). */
    blob(): Promise<Blob>;
    /** Fetch formData(): multipart/form-data (fields UTF-8 decoded without BOM; file parts become Files typed by their Content-Type, else text/plain) or application/x-www-form-urlencoded (bytes UTF-8 decoded whatever the charset; a leading "?" is kept). Any other MIME type, a missing boundary or a malformed body rejects with TypeError. */
    formData(): Promise<FormData>;
    /** A stream body is tee()d: this response keeps one branch, the clone gets the other. TypeError once used or while the body stream is locked. Headers keep their guard. */
    clone(): Response;
    arrayBuffer(): Promise<ArrayBuffer>;
    bytes(): Promise<Uint8Array>;
    json(): Promise<unknown>;
    /** UTF-8 decode of the bytes (a leading BOM is dropped; invalid sequences become U+FFFD). */
    text(): Promise<string>;
}
interface ResponseConstructor {
    /** body: see RequestInit.body (no duplex needed for a ReadableStream). */
    new (body?: unknown, init?: ResponseInit): Response;
    /** A network-error response: type "error", status 0, statusText "", immutable empty headers, null body. */
    error(): Response;
    /** A redirect to the parsed URL (TypeError if invalid) with immutable headers holding Location; status 301, 302 (default), 303, 307 or 308, else RangeError. */
    redirect(url: string | URL, status?: number): Response;
    /** JSON.stringify(data) as UTF-8 with content-type "application/json" (unless init.headers sets one); TypeError when data does not serialize. */
    json(data: unknown, init?: ResponseInit): Response;
}
declare const Response: ResponseConstructor;

/** W3C File API Blob (libc-blob.c). An immutable rope: a list of (store, offset, length) slices over refcounted byte stores; one slice is inline, more grow 4, 8, 16... slots.
 *  Construction copies string/BufferSource parts once into one new store per run of adjacent parts; a Blob part of >= 1024 bytes is shared by reference (no copy), a smaller one is copied into the run.
 *  slice() is O(number of slices) and never copies bytes. Reads (text/arrayBuffer/bytes) allocate the result once and fill it from the slices; stream() enqueues fresh 64 KiB chunks, or fills a BYOB reader's buffer in place.
 *  A string part is converted as a USVString (lone surrogate -> U+FFFD), encoded as UTF-8; a SharedArrayBuffer part throws TypeError. type: any char outside U+0020..U+007E makes it "", else ASCII-lowercased.
 *  endings: "native" turns CRLF/CR/LF in string parts into "\n" (POSIX) or "\r\n" (Windows); "transparent" (default) keeps them; anything else throws TypeError.
 *  Memory: an arrayBuffer()/bytes() result and every stream() chunk move off the native ledger when returned (they count toward the engine heap), so repeated reads never leave nativeSize high or hit a false out-of-memory under setNativeMemoryLimit. */
interface Blob {
    readonly size: number;
    readonly type: string;
    /** start/end are clamped long long, negative values count from the end; contentType is normalized like type. Always returns a Blob (never a File). */
    slice(start?: number, end?: number, contentType?: string): Blob;
    /** A byte ReadableStream (type "bytes") that holds its own references to the stores, so the Blob may be collected while it is read. */
    stream(): ReadableStream<Uint8Array>;
    /** stream() piped through a TextDecoderStream (UTF-8, ignoring any charset in type). */
    textStream(): ReadableStream<string>;
    /** UTF-8 decode (a leading BOM is dropped, invalid bytes become U+FFFD); an all-ASCII blob is copied straight into a one-byte string. */
    text(): Promise<string>;
    arrayBuffer(): Promise<ArrayBuffer>;
    bytes(): Promise<Uint8Array>;
}
interface BlobPropertyBag {
    type?: string;
    endings?: "transparent" | "native";
}
interface BlobConstructor {
    readonly prototype: Blob;
    new (blobParts?: Iterable<ArrayBuffer | ArrayBufferView | Blob | string>, options?: BlobPropertyBag): Blob;
}
declare const Blob: BlobConstructor;

/** W3C File: a Blob with a name and lastModified (ms since epoch; default the construction time). The name is kept as given. */
interface File extends Blob {
    readonly name: string;
    readonly lastModified: number;
    readonly webkitRelativePath: string;
}
interface FilePropertyBag extends BlobPropertyBag {
    lastModified?: number;
}
interface FileConstructor {
    readonly prototype: File;
    new (fileBits: Iterable<ArrayBuffer | ArrayBufferView | Blob | string>, fileName: string, options?: FilePropertyBag): File;
}
declare const File: FileConstructor;

/** W3C FileReader. Each readAs* sets readyState LOADING and queues one task per event: loadstart, progress (only for a non-empty blob), then load (or error) and loadend in one task;
 *  pending microtasks run after each listener, as in a browser. A readAs* call while LOADING throws DOMException InvalidStateError. abort() fires abort and loadend synchronously; a read started
 *  in an abort or load listener suppresses the old read's loadend. readAsText(blob, label): a BOM (UTF-8, UTF-16BE/LE) wins, then the label, then the charset in blob.type, then UTF-8;
 *  UTF-8 and UTF-16 are decoded in C, other labels through the global TextDecoder. Events are plain objects { type, target, currentTarget, loaded, total, lengthComputable, isTrusted }
 *  (there is no Event/EventTarget global); addEventListener supports the capture and once options and handleEvent objects, and only the six FileReader event types. */
interface FileReader {
    readonly readyState: 0 | 1 | 2;
    readonly result: string | ArrayBuffer | null;
    readonly error: DOMException | null;
    readonly EMPTY: 0;
    readonly LOADING: 1;
    readonly DONE: 2;
    onloadstart: ((ev: ProgressEventLike) => unknown) | null;
    onprogress: ((ev: ProgressEventLike) => unknown) | null;
    onload: ((ev: ProgressEventLike) => unknown) | null;
    onabort: ((ev: ProgressEventLike) => unknown) | null;
    onerror: ((ev: ProgressEventLike) => unknown) | null;
    onloadend: ((ev: ProgressEventLike) => unknown) | null;
    readAsArrayBuffer(blob: Blob): void;
    readAsBinaryString(blob: Blob): void;
    readAsText(blob: Blob, encoding?: string): void;
    /** "data:<type or application/octet-stream>;base64,<base64>". */
    readAsDataURL(blob: Blob): void;
    abort(): void;
    addEventListener(type: string, listener: unknown, options?: boolean | { capture?: boolean; once?: boolean }): void;
    removeEventListener(type: string, listener: unknown, options?: boolean | { capture?: boolean }): void;
}
interface ProgressEventLike {
    readonly type: string;
    readonly target: FileReader;
    readonly currentTarget: FileReader;
    readonly loaded: number;
    readonly total: number;
    readonly lengthComputable: boolean;
    readonly isTrusted: boolean;
}
interface FileReaderConstructor {
    readonly prototype: FileReader;
    new (): FileReader;
    readonly EMPTY: 0;
    readonly LOADING: 1;
    readonly DONE: 2;
}
declare const FileReader: FileReaderConstructor;

/** WebIDL DOMException: prototype chain reaches Error.prototype; code is the legacy code for the name (0 for names without one); the 25 legacy *_ERR constants sit on the constructor and prototype. */
interface DOMException extends Error {
    readonly name: string;
    readonly message: string;
    readonly code: number;
}
interface DOMExceptionConstructor {
    readonly prototype: DOMException;
    new (message?: string, name?: string): DOMException;
}
declare const DOMException: DOMExceptionConstructor;

/** The WHATWG URL class (with parse, canParse and join statics). It is a web global, so it is core (libc-url.c, libc-url-accessors.c, B1d): present in every build, with or without dyna:url; dyna:url re-exports the core constructor, so the global and the export are one object in every context and Worker. */
declare const URL: typeof import("dyna:url").URL & {
    /** "blob:<location.origin or null>/<uuid v4>"; the Blob is kept in a per-context Map until revokeObjectURL (fetch does not resolve blob: URLs). TypeError for a non-Blob. */
    createObjectURL(blob: Blob): string;
    revokeObjectURL(url: string): void;
};
/** WHATWG query list, THE same class the dyna:url module exports. It is core (libc-search-params.c, B1c): present in every build, with or without dyna:url; dyna:url re-exports the core constructor, so the global and the export are one object in every context and Worker. */
declare const URLSearchParams: typeof import("dyna:url").URLSearchParams;

/** The global fetch: the SAME function as dyna:http's — see its notes on redirects and dropped credentials. Request, Response, FormData and the multipart and urlencoded bodies are core (no dyna:url or dyna:http needed; URLs parse with the core WHATWG parser, never through globalThis.URL); only the transport is optional: a build without the "http" module rejects every network fetch with a TypeError naming "http", and a build without the optional "tls" module rejects https (also a redirect to https) with a TypeError naming "tls". Headers, FormData, AbortSignal/AbortController, Request, Response, the redirect policy and the body readers are C classes installed with no compile (libc-fetch.c); only the per-request await loop is JS, compiled once per context on the first fetch() call. A body reader always returns a promise (a bad body rejects, never throws). */
declare const fetch: typeof import("dyna:http").fetch;

/** WHATWG UTF-8 encoder: a lone surrogate becomes U+FFFD (EF BF BD); dyna:bytes' Text#toBytes keeps it as WTF-8. */
declare class TextEncoder {
    constructor();
    readonly encoding: string;
    encode(input?: string): Uint8Array;
    encodeInto(input: string, dest: Uint8Array): { read: number; written: number };
}
declare class TextDecoder {
    /** Every WHATWG Encoding label (default "utf-8"; trimmed, ASCII case-insensitive; .encoding is the canonical name): utf-8, utf-16le/utf-16be (label "utf-16" is utf-16le), the 28 single-byte encodings (windows-1252 for latin1/iso-8859-1/ascii/us-ascii, iso-8859-2..16, koi8-r/u, windows-874/1250..1258, ibm866, macintosh, x-mac-cyrillic), gbk, gb18030, big5, euc-jp, iso-2022-jp, shift_jis, euc-kr and x-user-defined. The replacement labels (replacement, iso-2022-kr, hz-gb-2312, iso-2022-cn, ...) and unknown labels are a RangeError.
     *  Legacy decoders run in C (src/core/dyn-textdec.c) over index tables generated from the WHATWG/WPT index data by tools/js/gen-textdec-index.js (src/core/dyn-textdec-index.h, checked by check-generated): gb18030 is the GB18030-2022 index (Go x/text pre-2022 table + the 18 WPT 2022 code points + PUA from iconv, asserted to land in U+E000..F8FF). The full jis0208/jis0212/euc-kr/big5 tables are checked against the WPT legacy-mb-* fixtures (tests/wpt/test_wpt_legacy_decode.js).
     *  A BOM is stripped only for utf-8 and utf-16 (unless ignoreBOM). Output is a Latin-1 (8-bit) string when every code unit is below U+0100, a UTF-16 string otherwise; the input view is read in place (no copy), one pass into a code-unit buffer of input+16 units. */
    constructor(encoding?: string, opts?: { fatal?: boolean; ignoreBOM?: boolean });
    readonly encoding: string;
    readonly fatal: boolean;
    readonly ignoreBOM: boolean;
    /** Decodes with the decoder's encoding. {stream: true} continues the decoder's stream: an incomplete trailing sequence (and iso-2022-jp mode, utf-16 lead byte/surrogate) is carried into the next call; a call without it is FINAL (an unterminated tail, e.g. an odd utf-16 byte or a lone lead surrogate, becomes U+FFFD, or a TypeError when fatal). */
    decode(buffer?: Uint8Array | ArrayBuffer, options?: { stream?: boolean }): string;
}

/** WHATWG Streams (https://streams.spec.whatwg.org), Encoding streams and Compression streams as globals.
 *  The 17 globals are lazy: the first access to ANY of them registers the native (C) classes once per realm (well under a millisecond, no JS is evaluated), so programs that never touch them pay nothing.
 *  Every algorithm runs in C; promise reactions the spec takes with no derived promise ("upon") are native reactions, so microtask order matches the spec job for job. Records (streams, readers, writers, controllers, pipes, tees) are native objects whose held values are GC-marked, so a stream cycle is collectable.
 *  Callback and start results are converted as WebIDL requires (a new promise adopting the value), so a callback that returns a promise settles two microtasks after it, as in browsers.
 *  Spec algorithms are followed step for step (queue-with-sizes, pull coalescing, BYOB pull-into descriptors, tee, pipeTo shutdown order); WebIDL conversions read dictionary members in lexicographic order, operations are enumerable, constructors throw on illegal construction.
 *  pipeTo's chunk write is queued as a microtask after each read (spec "queue a microtask to perform the remaining steps"); AbortError/DataCloneError are DOMExceptions. Streams are transferable through os.Worker postMessage transfer lists (see there); there is no MessageChannel. */
type QueuingStrategySize<T = unknown> = (chunk: T) => number;
interface QueuingStrategy<T = unknown> {
    /** Non-negative, not NaN; default 1 (0 for byte streams' readable side). */
    highWaterMark?: number;
    /** Chunk weight; must return a finite non-negative number or the stream errors. Not allowed on byte streams. */
    size?: QueuingStrategySize<T>;
}
interface UnderlyingSource<R = unknown> {
    /** Runs once, synchronously in the constructor; a returned promise delays the first pull. */
    start?(controller: ReadableStreamDefaultController<R> | ReadableByteStreamController): unknown;
    /** Called while desiredSize > 0 and again after each returned promise settles, never concurrently. */
    pull?(controller: ReadableStreamDefaultController<R> | ReadableByteStreamController): unknown;
    /** Called once on cancel with the reason; its rejection rejects cancel(). */
    cancel?(reason?: unknown): unknown;
    /** "bytes" makes a byte stream (ReadableByteStreamController, BYOB readers). */
    /** "owning" (Streams, tentative): enqueue(chunk, {transfer}) structured-clones the chunk with that transfer list; a non-empty transfer on any other stream is a TypeError. */
    type?: "bytes" | "owning";
    /** Byte streams only: a default reader's pull gets a byobRequest of this many bytes. */
    autoAllocateChunkSize?: number;
}
interface ReadableStreamReadResult<T> { done: boolean; value: T | undefined; }
interface ReadableStreamGetReaderOptions { mode?: "byob"; }
interface ReadableStreamIteratorOptions { preventCancel?: boolean; }
interface StreamPipeOptions {
    preventClose?: boolean;
    preventAbort?: boolean;
    preventCancel?: boolean;
    /** Aborting it shuts the pipe down with the signal's reason (abort/cancel the ends unless prevented). */
    signal?: AbortSignal;
}
interface ReadableWritablePair<R = unknown, W = unknown> { readable: ReadableStream<R>; writable: WritableStream<W>; }
/** WHATWG ReadableStream. Async-iterable (values()). dyna:stream fromFile/fromBytes piped to toFile, and toReadableStream piped to toWritableStream, take a native fast path (see dyna:stream). */
declare class ReadableStream<R = unknown> {
    constructor(underlyingSource?: UnderlyingSource<R>, strategy?: QueuingStrategy<R>);
    /** From an async iterable or a sync iterable (Symbol.asyncIterator first); cancel() calls the iterator's return(). */
    static from<T>(asyncIterable: AsyncIterable<T> | Iterable<T>): ReadableStream<T>;
    readonly locked: boolean;
    cancel(reason?: unknown): Promise<void>;
    getReader(): ReadableStreamDefaultReader<R>;
    /** BYOB readers exist only for byte streams (TypeError otherwise). */
    getReader(options: { mode: "byob" }): ReadableStreamBYOBReader;
    pipeThrough<T>(transform: ReadableWritablePair<T, R>, options?: StreamPipeOptions): ReadableStream<T>;
    pipeTo(destination: WritableStream<R>, options?: StreamPipeOptions): Promise<void>;
    /** Two branches; the source is canceled only when both are, with the reason [reason1, reason2]. Byte streams tee into byte streams, and branch 2 gets copies of the chunks. */
    tee(): [ReadableStream<R>, ReadableStream<R>];
    /** Locks the stream; breaking out of for-await cancels it unless preventCancel. */
    values(options?: ReadableStreamIteratorOptions): AsyncIterableIterator<R>;
    [Symbol.asyncIterator](options?: ReadableStreamIteratorOptions): AsyncIterableIterator<R>;
}
declare class ReadableStreamDefaultReader<R = unknown> {
    constructor(stream: ReadableStream<R>);
    readonly closed: Promise<void>;
    cancel(reason?: unknown): Promise<void>;
    read(): Promise<ReadableStreamReadResult<R>>;
    /** Pending reads reject with a TypeError. */
    releaseLock(): void;
}
declare class ReadableStreamBYOBReader {
    constructor(stream: ReadableStream<Uint8Array>);
    readonly closed: Promise<void>;
    cancel(reason?: unknown): Promise<void>;
    /** view's buffer is TRANSFERRED (the passed view is detached); the result is a view of the same type over the moved buffer. min (elements, default 1) waits until at least that many are filled, except at close. */
    read<T extends ArrayBufferView>(view: T, options?: { min?: number }): Promise<ReadableStreamReadResult<T>>;
    releaseLock(): void;
}
declare class ReadableStreamDefaultController<R = unknown> {
    /** Not constructible (TypeError). */
    private constructor();
    readonly desiredSize: number | null;
    close(): void;
    enqueue(chunk?: R): void;
    error(e?: unknown): void;
}
declare class ReadableByteStreamController {
    private constructor();
    /** The pending BYOB (or autoAllocateChunkSize) request, else null. */
    readonly byobRequest: ReadableStreamBYOBRequest | null;
    readonly desiredSize: number | null;
    close(): void;
    /** The chunk's buffer is transferred into the stream (detached for the caller). */
    enqueue(chunk: ArrayBufferView): void;
    error(e?: unknown): void;
}
declare class ReadableStreamBYOBRequest {
    private constructor();
    readonly view: Uint8Array | null;
    /** bytesWritten bytes were written into view (0 only after close). */
    respond(bytesWritten: number): void;
    /** Same buffer region, possibly a transferred buffer of the same capacity. */
    respondWithNewView(view: ArrayBufferView): void;
}
interface UnderlyingSink<W = unknown> {
    start?(controller: WritableStreamDefaultController): unknown;
    /** One write at a time; a rejection errors the stream. */
    write?(chunk: W, controller: WritableStreamDefaultController): unknown;
    close?(): unknown;
    abort?(reason?: unknown): unknown;
    type?: undefined;
}
/** WHATWG WritableStream. */
declare class WritableStream<W = unknown> {
    constructor(underlyingSink?: UnderlyingSink<W>, strategy?: QueuingStrategy<W>);
    readonly locked: boolean;
    abort(reason?: unknown): Promise<void>;
    close(): Promise<void>;
    getWriter(): WritableStreamDefaultWriter<W>;
}
declare class WritableStreamDefaultWriter<W = unknown> {
    constructor(stream: WritableStream<W>);
    readonly closed: Promise<void>;
    readonly desiredSize: number | null;
    /** Resolves when desiredSize > 0 (backpressure released). */
    readonly ready: Promise<void>;
    abort(reason?: unknown): Promise<void>;
    close(): Promise<void>;
    releaseLock(): void;
    write(chunk?: W): Promise<void>;
}
declare class WritableStreamDefaultController {
    private constructor();
    /** Aborted when the stream is aborted, with the abort reason. */
    readonly signal: AbortSignal;
    error(e?: unknown): void;
}
interface Transformer<I = unknown, O = unknown> {
    start?(controller: TransformStreamDefaultController<O>): unknown;
    /** Default: enqueue the chunk unchanged. */
    transform?(chunk: I, controller: TransformStreamDefaultController<O>): unknown;
    flush?(controller: TransformStreamDefaultController<O>): unknown;
    /** Called when the readable side is canceled or the writable side aborted. */
    cancel?(reason?: unknown): unknown;
    readableType?: undefined;
    writableType?: undefined;
}
/** WHATWG TransformStream: writes wait while the readable side is full (initial backpressure: the readable strategy defaults to highWaterMark 0). */
declare class TransformStream<I = unknown, O = unknown> {
    constructor(transformer?: Transformer<I, O>, writableStrategy?: QueuingStrategy<I>, readableStrategy?: QueuingStrategy<O>);
    readonly readable: ReadableStream<O>;
    readonly writable: WritableStream<I>;
}
declare class TransformStreamDefaultController<O = unknown> {
    private constructor();
    readonly desiredSize: number | null;
    enqueue(chunk?: O): void;
    error(reason?: unknown): void;
    terminate(): void;
}
/** Weighs chunks by chunk.byteLength. */
declare class ByteLengthQueuingStrategy {
    constructor(init: { highWaterMark: number });
    readonly highWaterMark: number;
    /** The same function for every instance; its name is "size". */
    readonly size: (chunk: ArrayBufferView) => number;
}
/** Weighs every chunk as 1. */
declare class CountQueuingStrategy {
    constructor(init: { highWaterMark: number });
    readonly highWaterMark: number;
    readonly size: (chunk?: unknown) => 1;
}
/** Strings in, UTF-8 Uint8Array chunks out; a surrogate pair split across chunks is joined, a lone surrogate becomes U+FFFD. */
declare class TextEncoderStream {
    constructor();
    readonly encoding: "utf-8";
    readonly readable: ReadableStream<Uint8Array>;
    readonly writable: WritableStream<string>;
}
/** BufferSource chunks in, strings out; multi-byte sequences split across chunks are joined. Labels as TextDecoder; fatal makes invalid input error the stream with a TypeError. */
declare class TextDecoderStream {
    constructor(label?: string, options?: { fatal?: boolean; ignoreBOM?: boolean });
    readonly encoding: string;
    readonly fatal: boolean;
    readonly ignoreBOM: boolean;
    readonly readable: ReadableStream<string>;
    readonly writable: WritableStream<ArrayBufferView | ArrayBuffer>;
}
/** "deflate" is zlib framing (RFC 1950), "deflate-raw" raw DEFLATE (RFC 1951), "gzip" RFC 1952, "brotli" RFC 7932 (darwin only: Apple libcompression; a TypeError elsewhere); dynajs extensions: "zstd" (RFC 8878 frames with a content checksum, level 3; only when built with libzstd, else a TypeError) and "lz4" (LZ4 frame format, 64 KiB independent blocks, level 1; encoder output does not depend on how the input was chunked; decoding verifies an optional content checksum); any other string (case-sensitive) is a TypeError. Decompression of the three DEFLATE formats runs a portable resumable inflater, so data after the end is handled chunk by chunk (a TypeError, except gzip's node-compatible multi-member and NUL-padding rules; see DecompressionStream). */
type CompressionFormat = "deflate" | "deflate-raw" | "gzip" | "brotli" | "zstd" | "lz4";
/** BufferSource chunks in, compressed Uint8Array chunks out, driven synchronously by the native dyna:stream codecs (no promise per chunk inside the codec). "gzip", "deflate" and "deflate-raw" share one streaming DEFLATE encoder in C on every platform: zlib level-6 lazy matching (128-step hash chains on a 4-byte hash, lazy threshold 8) with a 32 KiB window carried across chunks, dynamic Huffman blocks per 16 KiB of input, and a hardware CRC-32 for gzip. Output depends only on the bytes, never on how they were chunked; input is held until 16 KiB plus 269 bytes of lookahead are buffered (or the stream closes), so a short write produces no chunk yet. Memory: about 1 KB per stream until data arrives, about 49 KB while under 16 KiB is buffered, then about 240 KB (64 KB hash head, 64 KB chain links, 49 KB window buffer, up to 64 KB of tokens) until close, when it is freed. "brotli" uses Apple libcompression (darwin only). */
declare class CompressionStream {
    constructor(format: CompressionFormat);
    readonly readable: ReadableStream<Uint8Array>;
    readonly writable: WritableStream<ArrayBufferView | ArrayBuffer>;
}
/** The inverse of CompressionStream. Corrupt input, a checksum mismatch (gzip CRC-32, zlib Adler-32) or a truncated stream errors both sides with a TypeError. "deflate", "deflate-raw", "brotli", "zstd" and "lz4": any data after the end is a TypeError; the bytes decoded before it are enqueued first (as the spec requires, the error then resets the queue, so a reader sees the part of that prefix it had already pulled). "gzip" deliberately deviates from the Compression Streams spec (which makes any data after the end a TypeError), by owner choice, to match node's zlib.gunzip (checked against node v26 gunzipSync and createGunzip, whole input and chunks of 1, 3 and 7 bytes): after a member's 8-byte trailer, a 0x00 byte ends decoding and everything after it is ignored (NUL padding, even a later valid member); any other byte starts a new member that is decoded and appended (concatenated gzip, as `cat a.gz b.gz`), so it must be a complete valid member: a bad header (garbage such as 01 02 03 04, or a lone 1f), a truncated member or a CRC-32/ISIZE mismatch is a TypeError, as node throws there. WPT cost: decompression-corrupt-input "trailing junk for 'gzip'" (one 0x00 appended) fails, and decompression-extra-input times out (its gzip case never closes the stream and waits for an error). The three DEFLATE formats are decoded in C (64-bit bit buffer, 10-bit root tables with subtables, word-wise match copy, hardware CRC-32 where available) and the stream calls the codec directly, with no JS call per chunk. One input chunk yields one output chunk per 96 KiB of decoded data at most (the spec allows any split; no chunk is empty). Memory: about 1.2 KB per stream until the first compressed byte arrives, then one 51 KB table block laid out root tables first (literal root, distance root, distance subtables, literal subtables), so a typical block touches about 6 KB at its start instead of two separate pages, plus a history/output buffer that grows 1 KiB, 2 KiB, ... up to 128 KiB. Measured on macOS (16 KiB pages), 10000 live streams each holding 1 KB of output: 256 MB peak RSS against 391 MB before v10b. */
declare class DecompressionStream {
    constructor(format: CompressionFormat);
    readonly readable: ReadableStream<Uint8Array>;
    readonly writable: WritableStream<ArrayBufferView | ArrayBuffer>;
}

interface Performance {
    now(): number;
}
declare const performance: Performance;

/** Each call formats, writes and flushes at once (unbuffered), after flushing any dyna:stream stdout()/stderr() bytes still buffered, so the output keeps call order. log/info/debug go to stdout; trace/warn/error/assert to stderr. */
interface Console {
    log(...args: unknown[]): void;
    info(...args: unknown[]): void;
    debug(...args: unknown[]): void;
    trace(...args: unknown[]): void;
    warn(...args: unknown[]): void;
    error(...args: unknown[]): void;
    assert(cond: unknown, ...args: unknown[]): void;
}
declare const console: Console;

/** Prints values to stdout. */
declare function print(...args: unknown[]): void;

/** The engine's own argument vector. */
declare const scriptArgs: string[];

/** Standard timers; return a numeric id. A positive setTimeout delay is a minimum: the callback never runs before `ms` full milliseconds of monotonic time have passed since the call (deadlines are kept in microseconds and the loop waits with a microsecond timeout, so it runs as soon as the delay has passed, never early). Waits under 1 s are exempt from OS timer coalescing (macOS: an EVFILT_TIMER with NOTE_CRITICAL on the reactor kqueue, or on a per-thread deadline kqueue polled with the loop fds; Linux: epoll_pwait2 / ppoll), so setTimeout(1) fires about 1.02 ms after arming and setTimeout(10) about 10.05 ms, where a plain kevent or poll timeout is stretched by the kernel (1.26 ms for 1, 6.3 ms for 5, 12.0 ms for 10 on macOS); waits of 1 s or more keep the default leeway. setInterval periods are not rounded, so they do not drift. */
declare function setTimeout(cb: (...args: unknown[]) => void, ms?: number, ...args: unknown[]): number;
declare function clearTimeout(id: number): void;
declare function setInterval(cb: (...args: unknown[]) => void, ms?: number, ...args: unknown[]): number;
declare function clearInterval(id: number): void;

/** Microtask scheduling: the callback runs before the next macrotask. */
declare function queueMicrotask(callback: () => void): void;

/** WHATWG base64 over LATIN-1 STRINGS, not UTF-8: btoa("é") is "6Q==" and chars above U+00FF throw; use dyna:encoding's Base64* for bytes. */
declare function atob(data: string): string;
declare function btoa(data: string): string;

/** The base Iterator constructor; Iterator.prototype carries the ES2025 helpers plus engine extras, so every runtime iterator chains map/filter/take/... */
interface IteratorConstructor {
    /** Wraps any iterable or iterator in the helper-equipped iterator. */
    from<T>(objects: Iterable<T> | Iterator<T>): IteratorObject<T>;
    /** Concatenates iterables into one iterator (ES2025 Iterator.concat). */
    concat<T>(...items: (Iterable<T> | Iterator<T>)[]): IteratorObject<T>;
    /** Zips iterables into arrays; mode "shortest" (default), "longest" (pads from `padding`, else undefined) or "strict" (unequal lengths throw).
     *  The result is an Iterator Helper: return() closes every open input, last to first. */
    zip<T extends readonly (Iterable<unknown> | Iterator<unknown>)[]>(iterables: T, options?: { mode?: "shortest" | "longest" | "strict"; padding?: Iterable<unknown> }): IteratorObject<{ [K in keyof T]: T[K] extends Iterable<infer U> ? U : (T[K] extends Iterator<infer U2> ? U2 : unknown) }>;
    /** Zips an object's enumerable own iterables (undefined values skipped) into null-prototype objects with the same keys; padding is keyed. */
    zipKeyed<O extends Record<string, Iterable<unknown> | Iterator<unknown>>>(iterables: O, options?: { mode?: "shortest" | "longest" | "strict"; padding?: Partial<Record<keyof O, unknown>> }): IteratorObject<{ [K in keyof O]: O[K] extends Iterable<infer U> ? U : (O[K] extends Iterator<infer U2> ? U2 : unknown) }>;
    /** start + step*i from start toward end, end exclusive unless inclusive (TC39 proposal-iterator.range); step defaults to 1 or -1; NaN, an infinite start or step, or a zero step throw RangeError. */
    range(start: number, end: number, optionOrStep?: number | { step?: number; inclusive?: boolean }): IteratorObject<number>;
    /** BigInt form of range; end may also be Infinity or -Infinity. */
    range(start: bigint, end: bigint | number, optionOrStep?: bigint | { step?: bigint; inclusive?: boolean }): IteratorObject<bigint>;
}
declare const Iterator: IteratorConstructor;

/** The helper-equipped iterator shape every runtime iterator has. */
interface IteratorObject<T, TReturn = unknown, TNext = unknown> extends Iterator<T, TReturn, TNext> {
    [Symbol.iterator](): IteratorObject<T, TReturn, TNext>;
    map<U>(fn: (value: T) => U): IteratorObject<U, TReturn, TNext>;
    filter(fn: (value: T) => boolean): IteratorObject<T, TReturn, TNext>;
    take(n: number): IteratorObject<T, TReturn, TNext>;
    drop(n: number): IteratorObject<T, TReturn, TNext>;
    flatMap<U>(fn: (value: T) => Iterable<U> | Iterator<U>): IteratorObject<U, TReturn, TNext>;
    takeWhile(pred: (value: T) => boolean): IteratorObject<T, TReturn, TNext>;
    dropWhile(pred: (value: T) => boolean): IteratorObject<T, TReturn, TNext>;
    scan<R>(fn: (acc: R, value: T) => R, seed: R): IteratorObject<R, TReturn, TNext>;
    intersperse(v: T): IteratorObject<T, TReturn, TNext>;
    compact(): IteratorObject<NonNullable<T>, TReturn, TNext>;
    dropRepeats(): IteratorObject<T, TReturn, TNext>;
    dropRepeatsWith(eq: (a: T, b: T) => boolean): IteratorObject<T, TReturn, TNext>;
    dropRepeatsBy<K>(key: (value: T) => K): IteratorObject<T, TReturn, TNext>;
    /** Full sliding windows of size n (windows(n) with a coerced size); n < 1 throws RangeError. */
    aperture(n: number): IteratorObject<T[], TReturn, TNext>;
    /** Consecutive arrays of n values, the last one shorter (chunks(n) with a coerced size); n < 1 throws RangeError. */
    splitEvery(n: number): IteratorObject<T[], TReturn, TNext>;
    /** Consecutive arrays of size values, the last one shorter (TC39 proposal-iterator-chunking); size must be an integral number in 1..2^32-1. */
    chunks(size: number): IteratorObject<T[], TReturn, TNext>;
    /** Overlapping arrays of size values; "allow-partial" yields one short window when the source has fewer values (TC39 proposal-iterator-chunking). */
    windows(size: number, undersized?: "only-full" | "allow-partial"): IteratorObject<T[], TReturn, TNext>;
    /** Yields [index, value] pairs. */
    withIndex(): IteratorObject<[number, T], TReturn, TNext>;
    /** Alias of withIndex. */
    enumerate(): IteratorObject<[number, T], TReturn, TNext>;
    /** Flattens one level of iterables; a primitive value throws TypeError (flatMap with the identity). */
    flat(): IteratorObject<T extends Iterable<infer U> ? U : never, TReturn, TNext>;
    /** Repeats the source forever, buffering one pass; an empty source yields nothing. */
    cycle(): IteratorObject<T, TReturn, TNext>;
    /** Yields the first value and then every step-th one; step < 1 or NaN throws RangeError. */
    stepBy(step: number): IteratorObject<T, TReturn, TNext>;
    /** Calls fn(value, index) for each value and yields the value unchanged. */
    tap(fn: (value: T, index: number) => void): IteratorObject<T, TReturn, TNext>;
    zipWith<U, R>(fn: (a: T, b: U) => R, other: Iterable<U>): IteratorObject<R, TReturn, TNext>;
    pluck<K extends keyof T>(key: K): IteratorObject<T[K], TReturn, TNext>;
    tee(n?: number): IteratorObject<T, TReturn, TNext>[];
    unique(): IteratorObject<T, TReturn, TNext>;
    uniq(): IteratorObject<T, TReturn, TNext>;
    uniqBy<K>(key: (value: T) => K): IteratorObject<T, TReturn, TNext>;
    /** Materializes the iterator (ES2025 Iterator.prototype.toArray). */
    toArray(): T[];
    forEach(fn: (value: T) => void): void;
    reduce<R>(fn: (acc: R, value: T) => R, seed: R): R;
    reduce(fn: (acc: T, value: T) => T): T;
    some(pred: (value: T) => boolean): boolean;
    every(pred: (value: T) => boolean): boolean;
    find(pred: (value: T) => boolean): T | undefined;
    findIndex(pred: (value: T) => boolean): number;
    includes(value: T): boolean;
    count(pred?: (value: T) => boolean): number;
    first(): T | undefined;
    head(): T | undefined;
    last(): T | undefined;
    nth(i: number): T | undefined;
    init(): IteratorObject<T, TReturn, TNext>;
    tail(): IteratorObject<T, TReturn, TNext>;
    sum(): number;
    average(): number;
    mean(): number;
    product(): number;
    min(): T | undefined;
    max(): T | undefined;
    none(pred: (value: T) => boolean): boolean;
    any(pred: (value: T) => boolean): boolean;
    all(pred: (value: T) => boolean): boolean;
    countBy<K extends string | number>(key: (value: T) => K): Record<K, number>;
    indexBy<K extends string | number>(key: (value: T) => K): Record<K, T>;
    groupBy<K extends string | number>(key: (value: T) => K): Record<K, T[]>;
    reduceBy<K extends string | number, R>(fn: (acc: R, value: T) => R, seed: R, key: (value: T) => K): Record<K, R>;
    /** [values where fn(value, index) is truthy, the rest]. */
    partition(fn: (value: T, index: number) => unknown): [T[], T[]];
    /** Collects the values into a Set. */
    toSet(): Set<T>;
    /** Collects [key, value] entries into a Map; a non-object entry throws TypeError. */
    toMap<K, V>(this: IteratorObject<readonly [K, V]>): Map<K, V>;
    /** Joins like Array.prototype.join: separator defaults to ",", undefined and null become "". */
    join(separator?: string): string;
    /** The first value whose key fn(value, index) is smallest under the < operator (strings compare as strings). */
    minBy<K>(fn: (value: T, index: number) => K): T | undefined;
    /** The first value whose key fn(value, index) is largest under the < operator. */
    maxBy<K>(fn: (value: T, index: number) => K): T | undefined;
}

/** ES2025 half-precision (binary16) typed array. */
declare const Float16Array: Float16ArrayConstructor;
interface Float16ArrayConstructor {
    readonly prototype: Float16Array;
    readonly BYTES_PER_ELEMENT: number;
    new (length?: number): Float16Array;
    new (array: ArrayLike<number> | Iterable<number>): Float16Array;
    new (buffer: ArrayBufferLike, byteOffset?: number, length?: number): Float16Array;
    of(...items: number[]): Float16Array;
    from(arrayLike: ArrayLike<number>): Float16Array;
    from<T>(arrayLike: ArrayLike<T>, mapFn: (v: T, i: number) => number): Float16Array;
    from(source: Iterable<number>, mapFn?: (v: number, i: number) => number): Float16Array;
}
interface Float16Array {
    readonly length: number;
    readonly buffer: ArrayBufferLike;
    readonly byteOffset: number;
    readonly byteLength: number;
    [index: number]: number;
    at(index: number): number | undefined;
    set(array: ArrayLike<number>, offset?: number): void;
    subarray(start?: number, end?: number): Float16Array;
    slice(start?: number, end?: number): Float16Array;
    map(fn: (value: number, index: number, array: Float16Array) => number): Float16Array;
    forEach(fn: (value: number, index: number, array: Float16Array) => void): void;
    fill(value: number, start?: number, end?: number): this;
    copyWithin(target: number, start?: number, end?: number): this;
    reverse(): this;
    sort(compareFn?: (a: number, b: number) => number): this;
    /** The element count is captured before `separator` is coerced, so a detach inside a separator toString still emits the remaining separators. */
    join(separator?: string): string;
    indexOf(value: number, fromIndex?: number): number;
    lastIndexOf(value: number, fromIndex?: number): number;
    includes(value: number, fromIndex?: number): boolean;
    every(fn: (value: number) => boolean): boolean;
    some(fn: (value: number) => boolean): boolean;
    find(fn: (value: number) => boolean): number | undefined;
    findIndex(fn: (value: number) => boolean): number;
    filter(fn: (value: number) => boolean): Float16Array;
    reduce<U>(fn: (acc: U, value: number, index: number, array: Float16Array) => U, seed: U): U;
    reduce(fn: (acc: number, value: number, index: number, array: Float16Array) => number): number;
    keys(): IterableIterator<number>;
    values(): IterableIterator<number>;
    entries(): IterableIterator<[number, number]>;
    [Symbol.iterator](): IterableIterator<number>;
}

/** Explicit resource management (TC39 proposal, test262 feature explicit-resource-management): `using x = v` and `await using x = v` (async functions, async generators and module top level) bind a const and dispose it when the enclosing block, loop iteration (`for (using x of it)`), function or module body exits by any completion: records run in reverse order, the dispose method is read once at the declaration, null/undefined are skipped, a non-object or a missing method is a TypeError at the declaration, and each disposal error wraps the previous one as SuppressedError(error = new, suppressed = previous). `await using` awaits each [Symbol.asyncDispose] (or [Symbol.dispose]) result inline, once per record; null records cost at most one tick and none after a real await in the same scope. `using` is rejected at the top level of scripts and eval code and directly in a case clause. Iterator.prototype[Symbol.dispose] and AsyncIterator.prototype[Symbol.asyncDispose] call return(). */
interface SuppressedError extends Error {
    readonly error: unknown;
    readonly suppressed: unknown;
}
declare const SuppressedError: {
    readonly prototype: SuppressedError;
    new (error?: unknown, suppressed?: unknown, message?: string): SuppressedError;
};

interface DisposableStack {
    /** Registers a resource whose [Symbol.dispose] runs at stack disposal; null/undefined pass through. */
    use<T>(value: T): T;
    /** Registers a non-resource value with an explicit disposer. */
    adopt<T>(value: T, onDispose: (value: T) => void): T;
    /** Registers a bare callback to run at disposal. */
    defer(onDispose: () => void): void;
    /** Moves the captured callbacks to a fresh stack and returns it. */
    move(): DisposableStack;
    dispose(): void;
    readonly disposed: boolean;
    [Symbol.dispose](): void;
}
declare const DisposableStack: {
    readonly prototype: DisposableStack;
    new (): DisposableStack;
};

interface AsyncDisposableStack {
    /** Registers a resource whose [Symbol.asyncDispose] (or [Symbol.dispose]) runs at stack disposal; null/undefined pass through. */
    use<T>(value: T): T;
    adopt<T>(value: T, onDispose: (value: T) => void): T;
    defer(onDispose: () => void | Promise<void>): void;
    move(): AsyncDisposableStack;
    disposeAsync(): Promise<void>;
    readonly disposed: boolean;
    [Symbol.asyncDispose](): Promise<void>;
}
declare const AsyncDisposableStack: {
    readonly prototype: AsyncDisposableStack;
    new (): AsyncDisposableStack;
};

/** Engine error for internal invariant failures and out-of-memory ("out of memory"; never null).
 * When the error object itself cannot be allocated, a per-realm preallocated instance is thrown (shared, mutable). */
interface InternalError extends Error {
    name: "InternalError";
}
declare const InternalError: {
    readonly prototype: InternalError;
    new (message?: string): InternalError;
};

/** A native optic: a reusable, composable getter/setter over one focus (property, index, path, or custom pair); view/set/over never mutate the source. */
interface Lens<S = any, A = any> {
    /** Reads the focus (undefined when absent). */
    view(source: S): A | undefined;
    /** A copy of source with the focus set. */
    set(value: A, source: S): S;
    /** A copy of source with the focus transformed. */
    over(fn: (value: A) => A, source: S): S;
}
declare const Lens: {
    readonly prototype: Lens<any, any>;
    /** Builds a custom lens from a getter and an immutable setter set(value, source) -> updated source. */
    new <S, A>(get: (source: S) => A, set: (value: A, source: S) => S): Lens<S, A>;
    <S, A>(get: (source: S) => A, set: (value: A, source: S) => S): Lens<S, A>;
    /** A lens over one property. */
    prop(name: string): Lens<any, any>;
    /** A lens over one array index (preserves the array shape). */
    index(index: number): Lens<any, any>;
    /** A lens over a nested path: "a.b.c" (dotted) or ["a", "b", "c"]. */
    path(path: string | string[]): Lens<any, any>;
    /** Builds a custom lens (same as the constructor). */
    lens<S, A>(get: (source: S) => A, set: (value: A, source: S) => S): Lens<S, A>;
    /** Reads through the lens. */
    view<S, A>(lens: Lens<S, A>, source: S): A | undefined;
    /** Sets through the lens (immutable copy). */
    set<S, A>(lens: Lens<S, A>, value: A, source: S): S;
    /** Transforms through the lens (immutable copy). */
    over<S, A>(lens: Lens<S, A>, fn: (value: A) => A, source: S): S;
};

/** @internal Loads and evaluates a script file into global scope (bootstrap helper; use import instead). */
declare function __loadScript(filename: string): unknown;


/** Resolves after `ms` milliseconds without blocking the event loop (os.sleep is the blocking form); never before `ms` full milliseconds of monotonic time have passed. */
declare function sleep(ms: number): Promise<void>;

/** Structured deep copy (HTML structuredClone) with an optional transfer list; failures are DataCloneError DOMExceptions. Same function as dyna:serialize's. */
declare function structuredClone(value: unknown, options?: { transfer?: Iterable<unknown> }): unknown;
/** WebIDL DOMException: name/message/code accessors on the prototype, the 25 legacy code constants on both, and Error.prototype as the prototype's prototype. Lazy global (created on first access). Thrown as AbortError/TimeoutError by AbortSignal, DataCloneError by structuredClone and stream transfer. */
declare class DOMException extends Error {
    constructor(message?: string, name?: string);
    readonly name: string;
    readonly message: string;
    readonly code: number;
}

/* ---- Atomics (built-in, includes one non-standard extension) ------ */

/** Atomics: read-modify-write ops take INTEGER typed arrays over a SharedArrayBuffer; BigInt64/BigUint64 use BigInt values; `index` is an element index. */
interface Atomics {
    /** `ta[index] += value`; returns the OLD value. */
    add(ta: AnyIntegerTypedArray, index: number, value: number): number;
    /** `ta[index] &= value`; returns the OLD value. */
    and(ta: AnyIntegerTypedArray, index: number, value: number): number;
    /** `ta[index] |= value`; returns the OLD value. */
    or(ta: AnyIntegerTypedArray, index: number, value: number): number;
    /** `ta[index] -= value`; returns the OLD value. */
    sub(ta: AnyIntegerTypedArray, index: number, value: number): number;
    /** `ta[index] ^= value`; returns the OLD value. */
    xor(ta: AnyIntegerTypedArray, index: number, value: number): number;
    /** `ta[index] = value`; returns the OLD value. */
    exchange(ta: AnyIntegerTypedArray, index: number, value: number): number;
    /** Writes `replacement` only when the current value === `expected`; returns the OLD value. */
    compareExchange(ta: AnyIntegerTypedArray, index: number,
        expectedValue: number, replacementValue: number): number;
    /** Atomic load; returns `ta[index]`. */
    load(ta: AnyIntegerTypedArray, index: number): number;
    /** Atomic store; returns `value`. */
    store(ta: AnyIntegerTypedArray, index: number, value: number): number;
    /** True for element sizes 1/2/4/8. */
    isLockFree(size: number): boolean;
    /** NON-STANDARD spin hint: one CPU pause instruction (x86 PAUSE, arm64 YIELD) per call; the argument is validated but is not a loop count. */
    pause(iterations?: number): undefined;
    /** Blocks until notified, the value changes, or `timeout` ms: "ok" | "timed-out" | "not-equal". Int32Array/BigInt64Array only; the main thread cannot block (TypeError). */
    wait(ta: Int32Array | BigInt64Array, index: number,
        value: number, timeout?: number): "ok" | "timed-out" | "not-equal";
    /** Wakes at most `count` (default all) waiters on ta[index]; returns the number woken. */
    notify(ta: AnyIntegerTypedArray, index: number, count?: number): number;
}

/* ---- core prototype extensions (project additions) --------------- */

/** String.prototype extensions (absent under --no-prototypes). */
interface String {
    /** A lazy Iterator over the string's CODE POINTS (an astral character is one entry); composes with map/filter/take/toArray. */
    lazy(): Iterator<string>;
    /** True when the string is empty. */
    isEmpty(): boolean;
    /** Strips a leading prefix when present. */
    trimPrefix(prefix: string): string;
    /** Strips a trailing suffix when present. */
    trimSuffix(suffix: string): string;
    /** Strips every character in `chars` from both ends. */
    trimChars(chars: string): string;
    /** True when any code unit of `set` occurs. */
    containsAny(set: string): boolean;
    /** The first position of any code unit of `set`, or -1. */
    indexOfAny(set: string): number;
    /** Every position where `sub` occurs, ascending, counting overlaps. */
    indexOfAll(sub: string): number[];
    /** ASCII-only case-insensitive equality (only 'A'..'Z' fold; non-ASCII pairs compare unequal). */
    equalsIgnoreCase(other: string): boolean;
    /** Code-point comparison after NFC normalization; `locales`/`options` accepted and ignored (no ICU collation). */
    localeCompare(that: string, locales?: string | string[], options?: Intl.CollatorOptions): number;
    /** Byte-wise comparison, -1, 0, or 1. */
    compareBytes(other: string): number;
    /** Splits into at most n pieces. */
    splitN(sep: string, n: number): string[];
    /** True when the string is empty or all whitespace. */
    isBlank(): boolean;
    /** The first n characters, or "". */
    first(n?: number): string;
    /** The last n characters, or "". */
    last(n?: number): string;
    /** Characters from `from` (inclusive) to `to` (exclusive). */
    from(from: number, to?: number): string;
    /** Characters up to `to` (exclusive). */
    to(to: number): string;
    /** The code points as an array. */
    chars(): string[];
    /** The UTF-8 byte values as an array. */
    codes(): number[];
    /** The reversed string. */
    reverse(): string;
    /** A new string with `text` inserted at index `i` (default end). */
    insert(text: string, i?: number): string;
    /** Removes the first occurrence of `text`. */
    remove(text: string): string;
    /** Removes every occurrence of `text`. */
    removeAll(text: string): string;
    /** Collapses internal whitespace runs and trims. */
    compact(): string;
    /** Caesar-shifts each ASCII letter by n (default 0). */
    shift(n?: number): string;
    /** Center-pads to `len` with `padding`. */
    pad(len: number, padding?: string): string;
    /** Uppercases the first character (each word when `all`); lowercases the rest when `lower`. */
    capitalize(lower?: boolean, all?: boolean): string;
    /** CamelCase to snake_case. */
    underscore(): string;
    /** CamelCase to dash-separated. */
    dasherize(): string;
    /** Underscores/dashes to spaces. */
    spacify(): string;
    /** snake_case/dash-case to UpperCamelCase (`upper` true, default) or lowerCamelCase. */
    camelize(upper?: boolean): string;
    /** Truncates to `len` characters from `from` ("left" | "middle" | "right", default right) with `ellipsis`. */
    truncate(len: number, from?: "left" | "middle" | "right", ellipsis?: string): string;
    /** Truncates at a word boundary within `len`; same `from`/`ellipsis` options as truncate. */
    truncateOnWord(len: number, from?: "left" | "middle" | "right", ellipsis?: string): string;
    /** Escapes the five HTML-significant characters & < > " ' (as &amp; &lt; &gt; &quot; &#39;), so the result is safe in element content and inside a quoted attribute value. */
    escapeHTML(): string;
    /** Unescapes HTML entities. */
    unescapeHTML(): string;
    /** Strips HTML tags. */
    stripTags(): string;
    /** The number of occurrences of `sub`. */
    count(sub: string): number;
    /** C strtod/strtoll semantics (leading whitespace, inf/nan, 0x; `base` 2..36 = integer form), NOT JS Number(); NaN when nothing parses. */
    toNumber(base?: number): number;
    /** snake_case to a human label. */
    humanize(): string;
    /** Title-cases words. */
    titleize(): string;
    /** Converts to a URL-friendly slug. */
    parameterize(): string;
    /** A naive plural form. */
    pluralize(): string;
    /** A naive singular form. */
    singularize(): string;
    /** Removes every element with tag `tagName` (all elements when omitted), content included. */
    removeTags(tagName?: string): string;
    /** Calls fn for each character. */
    forEach(fn: (ch: string) => void): void;
    /** Brace formatting: {0}/{name} from args; {{ }} escape; a {name} reads OWN properties only, and an unknown name renders empty. */
    format(...args: unknown[]): string;
    /** The words of the string. */
    words(): string[];
    /** The lines of the string. */
    lines(): string[];
    /** Base64 encodes the string's UTF-8 bytes. */
    encodeBase64(): string;
    /** Base64 decodes to a string. */
    decodeBase64(): string;
    /** Percent-encodes (encodeURI; encodeURIComponent when `param`). */
    escapeURL(param?: boolean): string;
    /** Percent-decodes: `param` selects decodeURIComponent, absence decodeURI. */
    unescapeURL(param?: boolean): string;
    /** Strips ANSI escape sequences. */
    stripAnsi(): string;
    /** The display width of the string (wide characters count twice). */
    displayWidth(options?: { ambiguousAsWide?: boolean }): number;
    /** Wraps the string to `width` columns with ANSI. */
    wrapAnsi(width: number, options?: { hard?: boolean; trim?: boolean }): string;
    /** The grapheme clusters as an array. */
    graphemes(): string[];
    /** True when the string's UTF-8 encoding is well-formed. */
    isWellFormed(): boolean;
    /** Replaces lone surrogates with U+FFFD. */
    toWellFormed(): string;
}

/** Array.prototype extensions (absent under --no-prototypes). */
interface Array<T> {
    /** True when the array is empty. */
    isEmpty(): boolean;
    /** The first element, or undefined. */
    first(): T | undefined;
    /** The last element, or undefined. */
    last(): T | undefined;
    /** The sum of numeric elements. */
    sum(): number;
    /** The mean of numeric elements -- the canonical spelling of the pair. */
    average(): number;
    /** LEGACY alias of average; prefer average. */
    mean(): number;
    /** Removes null/undefined elements. */
    compact(): NonNullable<T>[];
    /** The number of elements matching `value`, a predicate, or a RegExp; all elements when omitted. */
    count(matcher?: T | ((v: T) => boolean) | RegExp): number;
    /** True when no element satisfies the predicate. */
    none(pred: (v: T) => boolean): boolean;
    /** True when any element satisfies the predicate. */
    any(pred: (v: T) => boolean): boolean;
    /** True when every element satisfies the predicate. */
    all(pred: (v: T) => boolean): boolean;
    /** The minimum element, by mapper or property key (default identity). */
    min(map?: ((v: T) => number) | keyof T): T;
    /** The maximum element, by mapper or property key (default identity). */
    max(map?: ((v: T) => number) | keyof T): T;
    /** The first n elements. */
    take(n: number): T[];
    /** Everything after the first n elements. Fixed-size groups have no Array method: `arr.values().chunks(n).toArray()` (and `windows(n)` for sliding ones). */
    drop(n: number): T[];
    /** The last n elements. */
    takeLast(n: number): T[];
    /** Everything except the last n elements. */
    dropLast(n: number): T[];
    /** Returns a NEW array sorted stably by a key function (the receiver is not changed, unlike sort()). Numeric keys compare numerically (NaN last); strings by UTF-8 byte order; numbers before strings. */
    sortBy<K>(key: (v: T) => K): T[];
    /** The insertion index for a sorted array; `comparator` must match the array's sort order. */
    sortedIndexOf(value: T, comparator?: (a: T, b: T) => number): number;
    /** Groups elements by a key function into a Record. */
    groupBy<K extends string | number>(key: (v: T) => K): Record<K, T[]>;
    /** A fresh array in random order from the GLOBAL RNG; use a dyna:random Random for reproducible shuffles. */
    shuffle(): T[];
    /** n random elements without replacement (global RNG, as shuffle). */
    sample(n?: number): T[];
    /** The distinct elements, first occurrence kept; `map` picks the dedup key. */
    unique(map?: ((v: T) => unknown) | keyof T): T[];
    /** LEGACY alias of unique; prefer unique. */
    uniq(map?: ((v: T) => unknown) | keyof T): T[];
    /** The distinct elements by a key function. */
    uniqBy<K>(key: (v: T) => K): T[];
    /** The elements present in both arrays. Canonical spelling of the pair. */
    intersect(other: T[]): T[];
    /** LEGACY alias of intersect; prefer intersect. */
    intersection(other: T[]): T[];
    /** The elements of this array not in `other`. */
    difference(other: T[]): T[];
    /** A copy without elements present in `other`. */
    without(other: T[]): T[];
    /** The union of this array with `other`. */
    union(other: T[]): T[];
    /** [passing, failing] by predicate. */
    partition(pred: (v: T) => boolean): [T[], T[]];
    /** The values of a property per element. */
    pluck<K extends keyof T>(key: K): T[K][];
    /** Zips with another array into pairs. */
    zip<U>(other: U[]): [T, U][];
    zipWith<U, R>(fn: (a: T, b: U) => R, other: U[]): R[];
    /** Inserts `v` between every pair of elements. */
    intersperse(v: T): T[];
    /** Recursive flatten, guarded to a depth of 512 (deeper nesting is emitted as-is). */
    flatten(): T extends unknown[] ? T[number][] : T[];
    /** The matrix transpose (array of arrays). */
    transpose(): T[][];
    /** The Cartesian product with another array. */
    xprod<U>(other: U[]): [T, U][];
    /** Sliding windows of size n. n === 0 yields len+1 empty windows; n < 0 throws RangeError. */
    aperture(n: number): T[][];
    /** Chunks of exactly n elements (last may be short). */
    splitEvery(n: number): T[][];
    /** Splits at the given index into [left, right]. */
    splitAt(i: number): [T[], T[]];
    /** A copy with index i set to fn(a[i]). */
    adjust(i: number, fn: (v: T) => T): T[];
    /** A copy with index i set to `v`. */
    update(i: number, v: T): T[];
    /** Moves the element at from to to. */
    move(from: number, to: number): T[];
    /** Swaps two elements. */
    swap(i: number, j: number): T[];
    /** The element at i, or undefined. */
    nth(i: number): T | undefined;
    /** Everything except the last element. */
    init(): T[];
    /** Everything except the first element. */
    tail(): T[];
    /** The first element (LEGACY alias of first). */
    head(): T | undefined;
    takeWhile(pred: (v: T) => boolean): T[];
    dropWhile(pred: (v: T) => boolean): T[];
    takeLastWhile(pred: (v: T) => boolean): T[];
    dropLastWhile(pred: (v: T) => boolean): T[];
    /** Appends one element. */
    append(v: T): T[];
    /** Prepends one element. */
    prepend(v: T): T[];
    /** The elements failing the predicate. */
    reject(pred: (v: T) => boolean): T[];
    /** A copy with `v` inserted at `i`. */
    insert(i: number, v: T): T[];
    insertAll(i: number, values: T[]): T[];
    /** A copy with the element at `i` removed. */
    removeAt(i: number): T[];
    /** An object mapping this[i] (as key) to values[i]. */
    zipObj<U>(values: U[]): Record<string, U>;
    /** An object from [key, value] pairs. */
    fromPairs(): Record<string, T>;
    /** The median numeric element. */
    median(): number;
    /** The product of numeric elements. */
    product(): number;
    /** Running accumulation: [x0, f(x0,x1), ...]. */
    scan<R>(fn: (acc: R, v: T) => R, seed: R): R[];
    /** Counts elements per key. */
    countBy<K extends string | number>(key: (v: T) => K): Record<K, number>;
    /** Indexes elements by a key. */
    indexBy<K extends string | number>(key: (v: T) => K): Record<K, T>;
    /** A copy without the elements equal to `value`. */
    remove(value: T): T[];
    /** Removes every occurrence of `value`. */
    exclude(value: T): T[];
    /** Removes `count` elements starting at `from` (second argument is a COUNT, not an end index; clamped). */
    removeRange(from: number, count: number): T[];
    /** Splits at the first index where pred changes. */
    splitWhen(pred: (v: T) => boolean): [T[], T[]];
    /** Keeps the elements for which `pred(a, b)` holds for some b in `other`. */
    innerJoin(pred: (a: T, b: T) => boolean, other: T[]): T[];
    /** True when the array starts with the given prefix. */
    startsWith(prefix: T[]): boolean;
    endsWith(suffix: T[]): boolean;
    /** Flattens nested arrays exactly one level (NOT the recursive flatten). */
    unnest(): T[];
    /** Drops consecutive duplicates. */
    dropRepeats(): T[];
    dropRepeatsWith(eq: (a: T, b: T) => boolean): T[];
    dropRepeatsBy<K>(key: (v: T) => K): T[];
    /** Sorts by an array of comparators. */
    sortWith(comparators: ((a: T, b: T) => number)[]): T[];
    /** Union with a custom equality. */
    unionWith(eq: (a: T, b: T) => boolean, other: T[]): T[];
    differenceWith(eq: (a: T, b: T) => boolean, other: T[]): T[];
    /** Elements in exactly one of the arrays. */
    symmetricDifference(other: T[]): T[];
    symmetricDifferenceWith(eq: (a: T, b: T) => boolean, other: T[]): T[];
    /** Reduces per key. */
    reduceBy<K extends string | number, R>(fn: (acc: R, v: T) => R, seed: R, key: (v: T) => K): Record<K, R>;
    /** Transducer composition over the array. */
    transduce<R>(xf: unknown, fn: (acc: R, v: T) => R, seed: R): R;
    /** Converts into another structure via a transducer. */
    into(target: unknown, xf: unknown): unknown;
    /** Traverses applicatively. */
    sequence<U>(of: (v: T) => U): U;
    traverse<U>(fn: (v: T) => U, of: (v: T) => U): U;
    /** Index-aware map from `startIndex`; `loop` wraps around to index 0. */
    mapFromIndex<R>(startIndex: number, fn: (v: T, i: number) => R, context?: unknown): R[];
    mapFromIndex<R>(startIndex: number, loop: boolean, fn: (v: T, i: number) => R, context?: unknown): R[];
    forEachFromIndex(startIndex: number, fn: (v: T, i: number) => void, context?: unknown): void;
    forEachFromIndex(startIndex: number, loop: boolean, fn: (v: T, i: number) => void, context?: unknown): void;
    filterFromIndex(startIndex: number, fn: (v: T, i: number) => boolean, context?: unknown): T[];
    filterFromIndex(startIndex: number, loop: boolean, fn: (v: T, i: number) => boolean, context?: unknown): T[];
    findFromIndex(startIndex: number, fn: (v: T, i: number) => boolean, context?: unknown): T | undefined;
    findFromIndex(startIndex: number, loop: boolean, fn: (v: T, i: number) => boolean, context?: unknown): T | undefined;
    findIndexFromIndex(startIndex: number, fn: (v: T, i: number) => boolean, context?: unknown): number;
    findIndexFromIndex(startIndex: number, loop: boolean, fn: (v: T, i: number) => boolean, context?: unknown): number;
    someFromIndex(startIndex: number, fn: (v: T, i: number) => boolean, context?: unknown): boolean;
    someFromIndex(startIndex: number, loop: boolean, fn: (v: T, i: number) => boolean, context?: unknown): boolean;
    everyFromIndex(startIndex: number, fn: (v: T, i: number) => boolean, context?: unknown): boolean;
    everyFromIndex(startIndex: number, loop: boolean, fn: (v: T, i: number) => boolean, context?: unknown): boolean;
    reduceFromIndex<R>(startIndex: number, fn: (acc: R, v: T, i: number) => R, seed?: R): R;
    reduceRightFromIndex<R>(startIndex: number, fn: (acc: R, v: T, i: number) => R, seed?: R): R;
    /** A lazy Iterator over the array's elements; composes with the Iterator helpers. */
    lazy(): Iterator<T>;
}

/** Array static extensions. */
interface ArrayConstructor {
    /** Repeats `value` n times. */
    repeat<T>(value: T, n: number): T[];
    /** Creates an array from an async iterable. */
    fromAsync<T>(iterable: AsyncIterable<T> | Iterable<T | PromiseLike<T>>): Promise<T[]>;
}

/** Map.prototype extensions (get-or-insert). */
interface Map<K, V> {
    /** The stored value for `key`, inserting `value` (default undefined) when absent. */
    getOrInsert(key: K, value?: V): V;
    /** The stored value for `key`, inserting `fn(key)` when absent. */
    getOrInsertComputed(key: K, fn: (key: K) => V): V;
}

/** Set static extensions. */
interface SetConstructor {
    /** Groups the iterable into a Map of key -> values array in input order. */
    groupBy<K, T>(items: Iterable<T>, fn: (value: T) => K): Map<K, T[]>;
}

/** Number.prototype extensions (absent under --no-prototypes). */
interface Number {
    abs(): number;
    sqrt(): number;
    exp(): number;
    sin(): number;
    cos(): number;
    tan(): number;
    asin(): number;
    acos(): number;
    atan(): number;
    negate(): number;
    inc(): number;
    dec(): number;
    add(other: number): number;
    subtract(other: number): number;
    multiply(other: number): number;
    divide(other: number): number;
    modulo(other: number): number;
    pow(other: number): number;
    gt(other: number): boolean;
    gte(other: number): boolean;
    lt(other: number): boolean;
    lte(other: number): boolean;
    isInteger(): boolean;
    isOdd(): boolean;
    isEven(): boolean;
    isMultipleOf(other: number): boolean;
    /** The modulo with the sign of the divisor. */
    mathMod(other: number): number;
    clamp(lo: number, hi: number): number;
    /** The logarithm in base `base` (default e). */
    log(base?: number): number;
    /** Rounds half away from zero to `precision` decimal places (negative: tens/hundreds). */
    round(precision?: number): number;
    ceil(precision?: number): number;
    floor(precision?: number): number;
    /** The character for this code point. */
    chr(): string;
    /** Zero-pads to `place` digits (above 65536 is clamped); `sign` forces the sign; `base` is 2..36. */
    pad(place?: number, sign?: boolean, base?: number): string;
    /** The number as lowercase hex, zero-padded to `place` digits (same 65536 clamp). */
    hex(place?: number): string;
    /** Locale-style grouping formatting: place, thousands separator, decimal separator. */
    format(place?: number, thousands?: string, decimal?: string): string;
    /** A compact human abbreviation (1.2k, 3.4m; lowercase k/m/b/t). */
    abbr(precision?: number): string;
    /** SI-prefixed magnitude. */
    metric(precision?: number): string;
    /** Byte-count formatting. */
    bytes(precision?: number): string;
    /** 1st, 2nd, 3rd, ... */
    ordinalize(): string;
    /** The number formatted as a duration. */
    duration(): string;
    /** Calls fn(i) n times; returns the results (fn defaults to identity). */
    times<R = number>(fn?: (i: number) => R): R[];
    /** Iterates from this number up to `end` inclusive by `step`; returns the results, mapped by `fn` when given. */
    upto<R = number>(end: number, step?: number, fn?: (value: number, index: number) => R): R[];
    /** Iterates from this number down to `end` inclusive by `step`; returns the results, mapped by `fn` when given. */
    downto<R = number>(end: number, step?: number, fn?: (value: number, index: number) => R): R[];
}

/** Number static extensions. */
interface NumberConstructor {
    /** An ARRAY of numbers [start, end), EXCLUSIVE of end; mathx.linspace is the inclusive n-point grid. */
    range(start: number, end?: number, step?: number): number[];
}

/** Object static extensions: functional helpers over plain objects (pick, omit, path, merge, ...). */
interface ObjectConstructor {
    isObject(v: unknown): v is object;
    isArray(v: unknown): v is unknown[];
    isBoolean(v: unknown): v is boolean;
    isNumber(v: unknown): v is number;
    isString(v: unknown): v is string;
    isFunction(v: unknown): v is (...args: unknown[]) => unknown;
    isDate(v: unknown): v is Date;
    isRegExp(v: unknown): v is RegExp;
    isError(v: unknown): v is Error;
    isSet(v: unknown): v is Set<unknown>;
    isMap(v: unknown): v is Map<unknown, unknown>;
    isArguments(v: unknown): boolean;
    isNil(v: unknown): v is null | undefined;
    isNotNil(v: unknown): boolean;
    /** A short type name: "String", "Number", "Object", ... */
    type(v: unknown): string;
    /** `value` when not nil, else the default. */
    defaultTo<T>(def: T, value: unknown): T;
    /** The number of own enumerable properties. */
    size(obj: object): number;
    isEmpty(obj: object): boolean;
    /** Swaps keys and values. Canonical spelling of the pair. */
    invert(obj: object): Record<string, string>;
    /** LEGACY alias of invert; prefer invert. */
    invertObj(obj: object): Record<string, string>;
    objOf<K extends string, V>(key: K, value: V): Record<K, V>;
    /** An object of the picked keys. */
    pick(keys: string[], obj: object): Record<string, unknown>;
    /** An object without the given keys. */
    omit(keys: string[], obj: object): Record<string, unknown>;
    pickBy(pred: (value: unknown, key: string) => boolean, obj: object): Record<string, unknown>;
    /** [key, value] pairs. */
    toPairs(obj: object): [string, unknown][];
    /** An object from [key, value] pairs. */
    fromPairs(pairs: [string, unknown][]): Record<string, unknown>;
    /** A copy with a property set. */
    assoc(key: string, value: unknown, obj: object): Record<string, unknown>;
    /** A copy with a property removed. */
    dissoc(obj: object, key: string): Record<string, unknown>;
    /** Calls fn with obj and returns obj. */
    tap<T>(fn: (v: T) => void, value: T): T;
    /** A deep clone (a cyclic structure throws RangeError). */
    clone<T>(value: T): T;
    /** Deep structural equality; a cyclic operand overflows the C stack with a RangeError (as clone). */
    equals(a: unknown, b: unknown): boolean;
    /** Object.is identity. */
    identical(a: unknown, b: unknown): boolean;
    /** The value at a key. */
    prop(key: string, obj: object): unknown;
    propOr(def: unknown, key: string, obj: object): unknown;
    /** The values at several keys. */
    props(keys: string[], obj: object): unknown[];
    /** The value at a nested path. */
    path(path: string[], obj: object): unknown;
    pathOr(def: unknown, path: string[], obj: object): unknown;
    /** Values at several paths. */
    paths(paths: string[][], obj: object): unknown[];
    /** A copy with a nested path set. */
    assocPath(path: string[], value: unknown, obj: object): Record<string, unknown>;
    /** A copy with a nested path removed. */
    dissocPath(path: string[], obj: object): Record<string, unknown>;
    hasPath(path: string[], obj: object): boolean;
    /** True when the key is present anywhere on the chain. */
    has(key: string, obj: object): boolean;
    hasIn(key: string, obj: object): boolean;
    keysIn(obj: object): string[];
    valuesIn(obj: object): unknown[];
    propEq(value: unknown, key: string, obj: object): boolean;
    eqProps(key: string, a: object, b: object): boolean;
    pathEq(value: unknown, path: string[], obj: object): boolean;
    /** True when obj satisfies every {key: predicate}. */
    where(spec: Record<string, (v: unknown) => boolean>, obj: object): boolean;
    /** True when obj matches the {key: value} spec. */
    whereEq(spec: Record<string, unknown>, obj: object): boolean;
    /** Shallow merge where later sources win; mergeLeft is the first-wins variant. */
    mergeRight(a: object, b: object): Record<string, unknown>;
    /** LEGACY alias of mergeRight; prefer mergeRight. */
    merge(a: object, b: object): Record<string, unknown>;
    mergeLeft(a: object, b: object): Record<string, unknown>;
    /** Deep merge. */
    mergeDeepRight(a: object, b: object): Record<string, unknown>;
    mergeDeepLeft(a: object, b: object): Record<string, unknown>;
    /** The value at a path, or undefined. */
    get(obj: object, path: string | string[]): unknown;
    /** Sets a path IN PLACE and returns `obj`; only OWN properties are walked or created, so a path can never reach a prototype. */
    set(obj: object, path: string | string[], value: unknown): Record<string, unknown>;
    /** Fills missing keys with defaults. */
    defaults(defaults: object, obj: object): Record<string, unknown>;
    /** A copy with {key: fn} applied per key. */
    evolve(transformations: Record<string, (v: unknown) => unknown>, obj: object): Record<string, unknown>;
    mapObjIndexed<R>(fn: (value: unknown, key: string) => R, obj: object): Record<string, R>;
    forEachObjIndexed(fn: (value: unknown, key: string) => void, obj: object): void;
    /** A copy with keys transformed by fn. */
    mapKeys(fn: (key: string) => string, obj: object): Record<string, unknown>;
    /** Merge with a custom value combine. */
    mergeWith(fn: (a: unknown, b: unknown) => unknown, a: object, b: object): Record<string, unknown>;
    mergeWithKey(fn: (key: string, a: unknown, b: unknown) => unknown, a: object, b: object): Record<string, unknown>;
    /** A copy with one key transformed. */
    modify(key: string, fn: (v: unknown) => unknown, obj: object): Record<string, unknown>;
    modifyPath(path: string[], fn: (v: unknown) => unknown, obj: object): Record<string, unknown>;
    /** pick including absent keys as undefined. */
    pickAll(keys: string[], obj: object): Record<string, unknown>;
    /** Projects objects onto the given keys. */
    project(keys: string[], objs: object[]): Record<string, unknown>[];
    propSatisfies(pred: (v: unknown) => boolean, key: string, obj: object): boolean;
    pathSatisfies(pred: (v: unknown) => boolean, path: string[], obj: object): boolean;
    /** True when obj satisfies any of the where spec's predicates. */
    whereAny(spec: Record<string, (v: unknown) => boolean>, obj: object): boolean;
    /** A copy with keys renamed per {old: new}. */
    renameKeys(map: Record<string, string>, obj: object): Record<string, unknown>;
    propIs(type: Function, key: string, obj: object): boolean;
    /** Groups array items by a key function. */
    groupBy<K extends string | number>(items: unknown[], key: (item: unknown) => K): Record<K, unknown[]>;
}

/* Legacy accessors live on Object.prototype, not the constructor. */
interface Object {
    __defineGetter__(property: string, getter: (this: unknown) => unknown): void;
    __defineSetter__(property: string, setter: (this: unknown, value: unknown) => void): void;
    __lookupGetter__(property: string): ((this: unknown) => unknown) | undefined;
    __lookupSetter__(property: string): ((this: unknown, value: unknown) => void) | undefined;
}

/** Date.prototype extensions (absent under --no-prototypes). */
interface Date {
    isValid(): boolean;
    isToday(): boolean;
    isYesterday(): boolean;
    isTomorrow(): boolean;
    isFuture(): boolean;
    isPast(): boolean;
    isWeekday(): boolean;
    isWeekend(): boolean;
    isLeapYear(): boolean;
    isSunday(): boolean;
    isMonday(): boolean;
    isTuesday(): boolean;
    isWednesday(): boolean;
    isThursday(): boolean;
    isFriday(): boolean;
    isSaturday(): boolean;
    isJanuary(): boolean;
    isFebruary(): boolean;
    isMarch(): boolean;
    isApril(): boolean;
    isMay(): boolean;
    isJune(): boolean;
    isJuly(): boolean;
    isAugust(): boolean;
    isSeptember(): boolean;
    isOctober(): boolean;
    isNovember(): boolean;
    isDecember(): boolean;
    getWeekday(): number;
    getISOWeek(): number;
    daysInMonth(): number;
    isBefore(other: Date): boolean;
    isAfter(other: Date): boolean;
    isBetween(a: Date, b: Date): boolean;
    millisecondsSince(other: Date): number;
    millisecondsUntil(other: Date): number;
    millisecondsAgo(): number;
    millisecondsFromNow(): number;
    secondsSince(other: Date): number;
    secondsUntil(other: Date): number;
    secondsAgo(): number;
    secondsFromNow(): number;
    minutesSince(other: Date): number;
    minutesUntil(other: Date): number;
    minutesAgo(): number;
    minutesFromNow(): number;
    hoursSince(other: Date): number;
    hoursUntil(other: Date): number;
    hoursAgo(): number;
    hoursFromNow(): number;
    daysSince(other: Date): number;
    daysUntil(other: Date): number;
    daysAgo(): number;
    daysFromNow(): number;
    weeksSince(other: Date): number;
    weeksUntil(other: Date): number;
    weeksAgo(): number;
    weeksFromNow(): number;
    monthsSince(other: Date): number;
    monthsUntil(other: Date): number;
    monthsAgo(): number;
    monthsFromNow(): number;
    yearsSince(other: Date): number;
    yearsUntil(other: Date): number;
    yearsAgo(): number;
    yearsFromNow(): number;
    addMilliseconds(n: number): Date;
    addSeconds(n: number): Date;
    addMinutes(n: number): Date;
    addHours(n: number): Date;
    addDays(n: number): Date;
    addWeeks(n: number): Date;
    addMonths(n: number): Date;
    addYears(n: number): Date;
    beginningOfDay(): Date;
    endOfDay(): Date;
    beginningOfWeek(): Date;
    endOfWeek(): Date;
    beginningOfMonth(): Date;
    endOfMonth(): Date;
    beginningOfYear(): Date;
    endOfYear(): Date;
    /** Adds a {days?, months?, ...} duration. */
    advance(delta: Record<string, number>): Date;
    /** Subtracts a {days?, months?, ...} duration. */
    rewind(delta: Record<string, number>): Date;
    clone(): Date;
    /** Formats with a layout string. */
    format(layout: string): string;
    /** A relative phrase like "3 days ago". */
    relative(): string;
    /** The ISO 8601 string. */
    iso(): string;
    /** Legacy year accessors and GMT alias. */
    getYear(): number;
    setYear(year: number): number;
    toGMTString(): string;
}

/** RegExp static extensions. */
interface RegExpConstructor {
    /** Escapes a literal string for use in a RegExp. */
    escape(text: string): string;
    /** Legacy statics of the last successful built-in match in this realm (only `input` is settable); reading them through a subclass, or after a subclass match, throws TypeError. */
    lastMatch: string; lastParen: string; leftContext: string; rightContext: string; input: string;
    /** Aliases (`$_` input, `$&` lastMatch, `$+` lastParen, `` $` `` leftContext, `$'` rightContext) and `$1`-`$9`, the first nine groups. */
    "$_": string; "$&": string; "$+": string; "$`": string; "$'": string; $1: string; $2: string; $3: string; $4: string; $5: string; $6: string; $7: string; $8: string; $9: string;
}

/** Promise static extensions (await-dictionary). */
interface PromiseConstructor {
    /** Promise.all over an object's enumerable own properties; resolves to a null-prototype object with the same keys. */
    allKeyed<T extends object>(promises: T): Promise<{ [K in keyof T]: Awaited<T[K]> }>;
    /** Promise.allSettled over an object's enumerable own properties, keyed the same way. */
    allSettledKeyed<T extends object>(promises: T): Promise<{ [K in keyof T]: PromiseSettledResult<Awaited<T[K]>> }>;
}

/** Immutable ArrayBuffers: no write through any view succeeds, and the buffer cannot be detached, resized or transferred. */
interface ArrayBuffer {
    /** Whether this buffer is immutable. */
    readonly immutable: boolean;
    /** Moves the contents (optionally resized) into a new immutable buffer and detaches this one. */
    transferToImmutable(newLength?: number): ArrayBuffer;
    /** Copies bytes [start, end) into a new immutable buffer; this buffer is left as it is. */
    sliceToImmutable(start?: number, end?: number): ArrayBuffer;
}

/** RegExp.prototype extensions. A match has no step limit of its own: catastrophic backtracking such as /(a+)+$/ on a long near-miss runs until it finishes or `--timeout-ms` interrupts it (uncatchably, nonzero exit), so untrusted patterns or subjects need that flag. The backtrack stack is capped at 256 MB; past it a match throws InternalError("out of memory in regexp execution"). A greedy `X*` or `X+` whose X is one character matcher (a literal, `.`, `[\s\S]`, `\s`, `\S`, a class, incl. /i and /u forms) takes constant stack however long the run; any other repeated piece (a group, an alternation, a lazy or bounded quantifier) keeps at least one 24-byte frame per iteration open (64 bytes for a two-way alternation), so e.g. /^(?:a|b)*X/ fails past about 4M iterations. */
interface RegExp {
    /** Whether the v flag is set. */
    readonly unicodeSets: boolean;
}
/** dyna:async — promise combinators and bounded handoff structures: sleep, withTimeout, retry, Semaphore, Channel, pmap, Pool, debounce, throttle.
 *  Everything runs on the event-loop thread: this is CONCURRENCY-limited interleaving, not parallelism. Bad arguments throw synchronously.
 *  Native design (no JS objects per waiter): Semaphore waiters, Channel/Queue buffers, Channel blocked senders and Pool jobs live in power-of-two ring buffers inside the C object; acquire() hands out one callable native release object per grant; pmap keeps its results array and counters in one C record and subscribes to each job with a native promise reaction (no derived promise per item); debounce/throttle return one callable native object holding the timer, the pending call's receiver and arguments (a C array) and the throttle edge; cancel() and pending() live on its shared prototype, which inherits Function.prototype, so call/apply/bind work. Ring buffers grow by realloc (the wrapped head segment moves to the new end), so peak memory stays one buffer. The prototypes carry no internal helper methods. */
declare module "dyna:async" {

    /** On-demand concurrency without ...Async twins: `offload(fn, ...args)` calls the native dyna:* function `fn(...args)` and returns a Promise of its result. Each native names the stretch of its own work that is safe off the loop thread (it works on a private copy of its inputs and touches no JS value there); under offload that stretch runs on a worker thread while the event loop keeps serving, and the result is built back on the loop thread. A native without such a stretch still runs, on the loop thread, and its promise settles the same way. Called directly, the same function is unchanged: synchronous, same result, same errors. Offloadable today: every one-shot codec of dyna:compress (gzip, gunzip, zstd, unzstd, brotli, unbrotli, snappy, unsnappy, lz4Compress, lz4Decompress, lz4Frame, lz4Unframe, deflate, inflate; archives build JS per entry and stay on the loop); the dyna:crypto password KDFs (Bcrypt.hash/verify, Scrypt, PBKDF2, Argon2id.hash/verify), which already return Promises and run on the pool when called directly, so offload() of one adds nothing but resolves to the same value; every one-shot digest of dyna:hash and its Hex form (all of the module but the streaming Hasher and CDCChunks); dyna:file readFile, readBytes and writeFile (the open and read or write; the result string or bytes are built on the loop); dyna:csv parseDataFrame (the scan into typed columns and a string dictionary; the DataFrame is wrapped on the loop). Offloaded work runs as a job on the shared io pool sized by --io-threads (CPU work and file IO share it; no thread is created per call); with --io-threads 0 there is no pool and offloaded work runs inline on the loop thread, settling the promise the same way. The other parsers (JSON.parse, MessagePack, csv.parse, XML, YAML) and docdb queries are not offloadable: the parsers build JS values as they scan (measured: 40-80% of a parse is value building, interleaved with the scan), and a docdb query.s matches index live collection slots. A JS function (or a bound one) is a TypeError: running JS mid-call off the loop would interleave it with other callbacks. `fn` is called with no receiver, so a method (a docdb Collection query, CSVFile#toDataFrame) rejects with its own TypeError (e.g. "Collection object expected") and cannot be offloaded. Errors from `fn`, including argument errors, reject the promise. The input is copied when the call starts (and its ArrayBuffer cannot be transferred until the call ends), so changing the buffer afterwards does not change the result. Cost: about 7-10 us per call on top of the work (one pool hop each way), plus the input copy for a buffer input (about 20 us per MiB); it pays from about 50-100 us of work (gzip or SHA-256 of about 64 KiB). Very fast functions (XXH3_64, CRC32 at any size; any call under about 64 KiB) are better made directly, and BLAKE3 over 1 MiB already uses every core on its own. Error backtraces from the offloaded call stop at `fn`. */
    function offload<A extends unknown[], R>(fn: (...args: A) => R, ...args: A): Promise<R>;

    /** Resolves after `ms` milliseconds (setTimeout). */
    function sleep(ms: number): Promise<void>;

    /** Rejects if `p` does not settle within `ms` (`reason` replaces the message); p's late rejection never surfaces as unhandled. */
    function withTimeout<T>(p: PromiseLike<T>, ms: number, reason?: string): Promise<T>;

    /** Integer arguments must fit int32, as the original `(x | 0) === x` checks: Semaphore n, Pool n, Queue/Channel cap and pmap concurrency are integers 1..2147483647 (cap may also be Infinity), retry `retries` is 0..2147483647. Anything else is a TypeError (a non-number, NaN or out-of-range count) or a RangeError (an out-of-range cap), never truncated or wrapped. Timer delays (sleep, withTimeout, debounce/throttle ms, backoffMs) must be finite and non-negative (else TypeError); a delay above 2147483647 ms is clamped to 2147483647 by setTimeout. */
    /** Calls `fn` until it resolves: 1 attempt + `retries` (default 3), waiting backoffMs * factor^attempt; the LAST error surfaces. */
    function retry<T>(fn: () => T | PromiseLike<T>, opts?: {
        retries?: number;
        backoffMs?: number;
        factor?: number;
        onRetry?: (err: unknown, attempt: number) => void;
    }): Promise<T>;

    /** Counting semaphore with direct hand-off (no queue jumping); acquire() resolves a release function, run(fn) wraps acquire/release. */
    class Semaphore {
        constructor(n: number);
        get available(): number;
        get waiting(): number;
        acquire(): Promise<() => void>;
        run<T>(fn: (...args: unknown[]) => T | PromiseLike<T>, ...args: unknown[]): Promise<T>;
    }

    /** The synchronous bounded FIFO (Channel is the async MPSC). */
    class Queue {
        /** The synchronous bounded FIFO: `push` throws when closed or full, `tryPush` returns false, `shift` returns `undefined` when empty, `close()` freezes it. */
        constructor(cap?: number);
        get length(): number;
        get closed(): boolean;
        /** Throws when closed or full (no silent drop). */
        push(v: unknown): this;
        /** False when closed or full. */
        tryPush(v: unknown): boolean;
        /** The next value, or undefined when empty (any state). */
        shift(): unknown;
        close(): void;
    }

    /** MPSC channel: send() backpressures at capacity; recv() resolves {value, done:false}, then {done:true} after close() and a full drain. SINGLE consumer: a second concurrent for-await throws a TypeError at the loop. */
    class Channel<T = unknown> {
        constructor(cap?: number);
        get length(): number;
        get closed(): boolean;
        send(v: T): Promise<void>;
        recv(): Promise<{ value: T; done: false } | { done: true }>;
        close(): void;
        [Symbol.asyncIterator](): AsyncIterator<T>;
    }

    /** Maps `fn` over `items` with at most `concurrency` (default 8) in flight, results in ITEM order; the FIRST rejection rejects the whole pmap. */
    function pmap<T, R>(items: T[], fn: (item: T, index: number, items: T[]) => R | PromiseLike<R>,
                        opts?: { concurrency?: number }): Promise<R[]>;

    /** A long-lived scheduler with `n` submit slots (interleaving, not worker parallelism); a throwing job rejects its submit and frees its slot. */
    class Pool {
        constructor(n: number);
        get size(): number;
        get active(): number;
        get pending(): number;
        /** Queues a job; settles with its result once a slot ran it. */
        submit<T>(fn: () => T | PromiseLike<T>): Promise<T>;
    }

    /** Trailing debounce: the last call inside the `ms` window runs once at its edge; cancel() drops a pending call. */
    function debounce<A extends unknown[]>(fn: (...args: A) => void, ms: number):
        ((...args: A) => void) & { cancel(): void; pending(): boolean };

    /** Leading throttle with a trailing call: the first call runs now, the last call in the window runs at its edge. */
    function throttle<A extends unknown[]>(fn: (...args: A) => void, ms: number):
        ((...args: A) => void) & { cancel(): void; pending(): boolean };
}
