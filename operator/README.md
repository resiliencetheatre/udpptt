# operator

Small C/ncurses chat terminal for `ptt_wav_gate`. Received WAV recordings
are transcribed using the adjacent `watch-whisper.sh`; typed messages are
synthesized by `kokoro-offline` and queued as WAV files for transmission.

## Setup

Build (requires a C compiler, pkg-config and ncursesw development files):

```sh
make -C operator
```

For a new installation, copy the template and populate your local settings:

```sh
cp operator/operator.env.example operator/operator.env
chmod 600 operator/operator.env
```

Keep an existing configured `operator.env` when updating. It is ignored by Git;
`operator.env.example` is the shareable template. Put your server address,
transmitter ID, encryption key, preamble ID, input/output directories, state
file, and local speech-tool paths only in the private file.

The file uses literal `KEY=VALUE` lines. Do not add quotes, `export`, inline
comments, or shell expansions such as `$HOME` or `~`. Use absolute paths;
spaces, `#`, and other special characters after `=` are literal parts of the
value. Blank lines and lines beginning with `#` are ignored. Neither the
application nor the watcher executes the file as a shell script.

`operator` loads `operator.env` beside its executable. Select another file
with `--env-file FILE` or `OPERATOR_ENV_FILE`. Command-line options override
existing environment variables, which override file values. An absent default
file is allowed when settings are supplied through the environment; an
explicitly selected file must exist. Required missing values produce an error
naming the setting, without displaying its value.

The required settings for attaching to a gate are `OPERATOR_INPUT_DIR`,
`OPERATOR_OUTPUT_DIR`, and `WHISPER_MODEL`. Launching a gate also requires
`OPERATOR_SERVER`, `OPERATOR_TXID`, `OPERATOR_KEY`, `OPERATOR_PREAMBLE_ID`, and
`OPERATOR_STATE_FILE`. Input/output directories and the state file's immediate
parent are created as needed; their parents must already exist.

## Run

Launch the gate with your configured settings:

```sh
./operator/operator --start-gate
```

If the gate is already running, attach through its configured WAV directories:

```sh
./operator/operator
```

From this directory, use `make` and `./operator` instead. The watcher and
built gate are found relative to the executable. `OPERATOR_GATE` or
`--gate PROGRAM` overrides the gate executable. Gate launches use encryption;
the template exposes jitter, FEC, and receive-timeout settings as well.

## Chat and speech

Enter queues a message; you can keep typing while speech is being generated.
Backspace edits, Ctrl-U clears the input, Page Up/Down scroll history, and
Ctrl-Q, Ctrl-D or Ctrl-C exits. Terminal resizing and UTF-8 input are supported
(use a UTF-8 locale). The queue holds up to 32 messages, including the active
one; each message is limited to 1023 characters. The window keeps the latest
500 history entries. Editing is at the end of the input line.

`ME queued` means the text is waiting for Kokoro. `TX ready` means a complete
WAV was published to the gate's input queue; it is not a delivery receipt.
Kokoro runs serially, using the configured voice and language. Files are
created in a private staging subdirectory and atomically published under
unique names, preventing partial transmission or overwriting earlier speech.
The gate handles resampling and moves transmitted files into `input/sent`.
Kokoro failures are displayed and are not automatically retried.

The watcher waits for stable received WAV files, transcribes them, and archives
them under `output/archive`. It keeps its completion state across restarts.
Do not also run `watch-whisper.sh` separately against the same output directory.
Received history is reloaded on startup. Transcripts and helper diagnostics are
stored in `output/operator/transcriptions.log` and `output/operator/helpers.log`;
Whisper errors also go to `transcriptions.log.errors`. Here, `input` and `output`
mean your configured directories. Outgoing chat history is only held in memory.
Helper exits are reported in the chat; restart operator after correcting errors.

The standalone watcher also reads the adjacent `operator.env`, or the file
selected by `OPERATOR_ENV_FILE`. Positional watch/log/archive arguments still
work. Its default watch directory comes from `OPERATOR_OUTPUT_DIR`.

The watcher needs Bash and standard tools (`flock`, `find`, `sort`, `stat`,
`sha256sum`, `realpath`, `sed`). Kokoro and Whisper executable names can be
resolved through `PATH` or supplied as absolute paths. Their offline model
and voice assets must already be installed.

On exit, operator stops the helpers it started, including a gate started with
`--start-gate`. An external gate keeps running. Unsynthesized queued text is
cancelled; already published WAVs remain for the gate. Only one operator may
use a given output directory at a time.

## Test

Run the local integration test (temporary configuration, mock speech helpers
and gate; no network or private settings):

```sh
make -C operator check
```
