# Quickstart — set up Pigeon with your AI agent

The fastest way to get Pigeon running is to hand it to a coding agent (Claude
Code, or any agent with a shell). You don't need to download anything first — you
paste a short prompt, the agent reads this page, walks you through what it's about
to do, and only downloads the repo once you say where and give the go-ahead.

## Step 1 — paste this into your agent

```text
I want to set up Pigeon, a wireless USB-keyboard tool. Read this page over the
web and follow it exactly:
https://github.com/cobylai/pigeon-release/blob/main/QUICKSTART.md

Do not clone or download anything to my machine yet. Just read that page (and any
repo pages it points you to) over the web, then do what it says.
```

That's the whole paste. Everything else is instructions the agent reads from this
page.

> **You need an agent that can run commands on this computer.** Flashing the
> boards means running terminal commands and talking to USB devices, so use a
> coding agent with shell access on your own machine — the Claude Code **desktop
> app** or **CLI**, or an equivalent. A browser-only chat assistant can read this
> page and explain things, but it cannot flash hardware.

---

## Step 2 — what the agent does (agent: follow this exactly)

**If you are an AI agent that was just handed the prompt above, this section is
for you. Do not clone, download, build, or change anything on the user's machine
until Step 3 says so.**

1. **Greet the user**, roughly like this:

   > Welcome to Pigeon — the low-cost communication tool built around the
   > ESP32-S3 SuperNano. Would you like an overview before we continue? I can skip
   > straight to flashing, or we can walk through how it works first.

2. **Read these pages over the web** (do NOT clone the repo yet — reading a web
   page is fine, downloading to their machine is not):
   - `https://github.com/cobylai/pigeon-release/blob/main/README.md`
   - `https://github.com/cobylai/pigeon-release/blob/main/SETUP.md`
   - `https://github.com/cobylai/pigeon-release/blob/main/AI-BUILD.md`

3. **Follow the user's lead:**
   - Wants an overview / to walk through it → explain, in plain language, what
     Pigeon is, the two-board Nest/Pigeon setup, and the end-to-end flow (build →
     flash both boards → provision a shared key → assign roles → smoke test).
   - Wants to skip → go straight on toward flashing.
   - Either way, **answer any questions they have** using what you just read.

4. **Tell them what setup involves and what needs their hands:** the first flash
   of each blank board needs them to press BOOT/RESET and do one unplug/replug;
   everything else you handle, and later reflashes are hands-free. Also note the
   link key file `~/.pigeon_link_key` is a secret you'll never print or commit.

5. **Ask where to set it up** — which directory on their machine they want the
   repo cloned into.

## Step 3 — download and run (only after the user confirms)

6. **Only after** the user has had their questions answered and told you where to
   put it, download the repo to that location:

   ```bash
   git clone https://github.com/cobylai/pigeon-release.git <their-chosen-dir>
   ```

   From here the bundled skill at `.claude/skills/pigeon/` loads automatically.

7. **Check prerequisites before starting.** Confirm the tools this needs are
   present, and if any are missing, tell the user and point them at
   [SETUP.md](SETUP.md) rather than pushing on:
   - `git` and a C compiler (`cc`) — the compiler runs the host self-test.
   - `python3` with `pyserial` (`python3 -c "import serial"`; else `pip install pyserial`).
   - The ESP-IDF toolchain: `idf.py --version` after
     `. "$HOME/esp/esp-idf/export.sh"`. If it's not installed, follow SETUP.md's
     install and stop to tell the user if anything there needs their input.
   - Both boards visible on USB (`ls /dev/cu.usbmodem*`) with data-capable cables.

8. **Sanity-check communication early.** Before and after flashing, prove the
   pieces actually talk rather than assuming:
   - Host self-test (no hardware):
     `cc -o /tmp/t test_replay.c main/replay.c -Imain && /tmp/t`.
   - After the Nest is flashed and keyed: `./pigeon-send local-status` replies.
   - After roles are set: `./pigeon-send status` shows `paired=1` and
     `authfail=0`, then a smoke test — `./pigeon-send type --enter "pigeon online"`
     into a focused field on the target — confirms end to end.

9. **Ask how far to go.** Offer to either run the whole setup through to that
   smoke test, or stop at a checkpoint they name. The natural checkpoints are:
   prerequisites checked → firmware built → both boards flashed → keys
   provisioned → roles assigned → smoke test. Stop wherever they ask.

10. **Run it following [AI-BUILD.md](AI-BUILD.md)**, in order, verifying each step.
    Whenever a physical action is needed, STOP and tell the user exactly what to do
    — e.g. "hold BOOT, tap RESET, release BOOT, then tell me when done" for the
    first flash, and "unplug the board and plug it back in" afterward — then wait
    for their confirmation. Never assume a physical action happened.

---

Prefer to do it all by hand instead? See [SETUP.md](SETUP.md) for the manual
toolchain/build/flash steps, or [AI-BUILD.md](AI-BUILD.md) for the agent prompt
that runs the setup straight through.
