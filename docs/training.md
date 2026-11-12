# Training on your own computer

The goal is a neural-network evaluation trained only on Tessera's own games.
That needs about 100 million positions. The repository holds 8 million; the
rest is generated on your machine with the script below. It can be stopped and
continued as often as you like.

## Step 1: generate games (stop and resume freely)

### Windows

No compiler needed: GitHub builds `tessera.exe` automatically from the current
sources (`.github/workflows/windows-build.yml`). In PowerShell:

```powershell
winget install Python.Python.3.12        # skip if "python --version" already works
winget install Git.Git                   # skip if "git --version" already works; reopen PowerShell after installing
git clone https://github.com/caqyx/chessengine.git      # download the project
cd chessengine
curl.exe -L -o engine\tessera.exe https://github.com/caqyx/chessengine/releases/download/windows-build/tessera.exe   # the ready-made engine
python tools\gen_data.py                 # start generating
```

The download is built for CPUs with AVX2 (anything from about 2015 on). To get
a newer engine later, run the `curl.exe` line again after `git pull`.

Building it yourself is optional: install WSL (`wsl --install -d Ubuntu`) or
MSYS2 and follow the Linux steps.

### Linux

```bash
sudo apt install -y g++ make python3 git                      # or your distribution's equivalent
git clone https://github.com/caqyx/chessengine.git            # download the project
cd chessengine
make -C engine                                                # build the engine, optimised for your CPU
python3 tools/gen_data.py                                     # start generating
```

### Using it

```bash
python3 tools/gen_data.py                 # start, or continue where you stopped
# Ctrl+C                                  # stop: everything finished so far is already saved
python3 tools/gen_data.py --threads 6     # use fewer CPU threads if you want the PC to stay responsive
python3 tools/gen_data.py --target 30000000   # a different goal than the default 100 million positions
git pull && make -C engine                # later, Linux: get a newer engine before continuing (old data stays valid)
```

Before continuing a run with a newer engine, stop the generator first (Ctrl+C):
Windows cannot replace `tessera.exe` while it is running. The data already
generated stays valid and the run continues where it stopped.

What to expect:

- It prints how many positions you have, the speed, and the time left.
- By default it uses every CPU thread but one. On a 6-core/12-thread CPU expect
  a few thousand positions per second, so 100 million takes on the order of
  6 to 10 hours in total, split over as many sessions as you like.
- Data goes to `data/local/` as plain text, about 75 MB per million positions
  (roughly 7.5 GB for 100 million). The folder is ignored by git.
- Stopping loses at most the few batches in progress, a few seconds of work.
  A crash or power cut is handled the same way on the next start.
- Each game gives the two sides different random thinking budgets
  (1,000 to 10,000 positions per move), so stronger and weaker players meet.

### Starting a fresh round

Each network makes the engine stronger, and games from a stronger engine are
better training material. To generate a round with the newest engine without
mixing it with older games, give it its own folder (stop the generator first,
then download or build the newest engine):

```bash
python3 tools/gen_data.py --out data/round3    # new games into their own folder (Windows: data\round3)
```

A new folder starts from a random seed, so it does not replay the random
opening moves of earlier rounds. Keep the older folders: whether they help or
hurt is something to measure, not assume (step 2 shows how).

## Step 2: train the network (stop and resume freely)

Training runs on the CPU with all threads; no graphics card or Python packages
are needed. It is fast: about 350,000 positions per second per CPU thread here,
so a pass over 40 million positions takes well under a minute on a 12-thread CPU.

### Windows

```powershell
cd chessengine
git pull                                  # get the training script and the newest sources
curl.exe -L -o engine\tessera.exe https://github.com/caqyx/chessengine/releases/download/windows-build/tessera.exe   # newest engine (can load networks)
curl.exe -L -o engine\trainer.exe https://github.com/caqyx/chessengine/releases/download/windows-build/trainer.exe   # the trainer
python tools\train_net.py                 # convert the data, then train
```

### Linux / WSL

```bash
cd ~/chessengine
git pull                                  # get the training script and the newest sources
make -C engine && make -C engine trainer  # rebuild the engine (can load networks) and build the trainer
python3 tools/train_net.py                # convert the data, then train
```

### Using it

```bash
python3 tools/train_net.py                # start, or continue where you stopped
# Ctrl+C                                  # stop: it saves and exits within a second or two
python3 tools/train_net.py --fresh        # start over, e.g. after generating a lot more data
python3 tools/train_net.py --hidden 128   # a smaller, faster network (default 256, what the engine ships)
```

What happens:

1. Every data file is converted once to a compact form in `data/packed/`
   (the files in `data/` and everything in `data/local/`). Unchanged files are
   skipped on later runs.
2. The trainer makes several passes over the data. After each pass it tests the
   network on games it never trained on (2% of every file) and prints that as
   "held-out loss". Only a network that improves on it is written to
   `nets/tessera.nnue`, so training too long cannot make the saved network worse.
3. Progress is in `nets/checkpoint.bin`. Stopping in the middle of a pass is
   fine; the next run continues from the same step.

### Training on chosen folders

```bash
python3 tools/train_net.py --data data/round3 --out nets-round3                      # only the new round
python3 tools/train_net.py --data data data/local data/round3 --out nets-all         # everything
```

Each selection gets its own converted copy (`data/packed-...`) and its own
output folder, so both can be trained and then matched against each other.

### Is the network better? Measure it

The engine uses its built-in network unless told to load another one, so this compares your new network (engine 1) with the built-in one (engine 2):

```bash
pip install chess                         # match runner dependency (python-chess)
python3 tools/match.py --e1 engine/tessera --e2 engine/tessera \
    --opt1 "EvalFile=nets/tessera.nnue" --movetime 100 --games 200 --concurrency 4
# engine 1 = your new network, engine 2 = the built-in one; the Elo line is from engine 1's point of view
```

(On Windows use `engine\tessera.exe` and `nets\tessera.nnue`.)

What to expect, from the tests done while building this (300 games each at
20,000 nodes per move, network against the hand-written evaluation):

| Training positions | What the network was taught | Result for the network |
|---|---|---|
| 2 million | half game result, half search score | -263 Elo |
| 4 million | half game result, half search score | -147 Elo |
| 8 million | half game result, half search score | -147 Elo |
| 7 million | search score only | -56 Elo |
| 7 million | 80% search score, 20% game result (the default now) | -10 Elo [-42, +21] |
| 7 million | same, twice as many passes | +3 Elo [-28, +35] |
| 7 million | same as two rows up, 200 games at 100,000 nodes | 0 Elo [-36, +36] |

Two lessons: more data helps, and what the network is asked to predict matters
even more. With the default settings and only 7 million positions the network
is already level with the hand-written evaluation, so with 30 to 40 million it
has a good chance of passing it. The match above is what decides; if it is
still behind, generate more (step 1) and train again with `--fresh`.

## Optional: Stockfish evaluations from Lichess

Lichess publishes about 418 million positions evaluated by Stockfish, as a
public-domain (CC0) download: the "evaluations" file on
https://database.lichess.org/ (`lichess_db_eval.jsonl.zst`). It can be added
to the training data.

**It changes what the engine is.** A network trained on it learns partly from
Stockfish's judgment, so Tessera is then no longer "trained only on its own
games". If a network trained with it ships, the README has to say so.

```bash
pip install zstandard                                          # reads the compressed file
python3 tools/import_lichess.py path/to/lichess_db_eval.jsonl.zst   # calibrate, then import; Ctrl+C stops, same command continues
python3 tools/train_net.py --data data/round3 data/lichess --out nets-lichess   # train on your newest round plus Lichess
```

What the import does:

- **Calibration, once, about a minute:** Tessera searches 4,000 positions from the
  file and compares its scores with Stockfish's. That decides whether the file's
  scores are from White's side or the side to move (the database does not say),
  and converts Stockfish centipawns to Tessera's scale.
- **Import:** the file is decompressed on the fly. For each position it takes the
  deepest evaluation and its first move, as Lichess recommends. It keeps quiet
  positions only: not in check, first move not a capture or promotion, no mate
  scores, nothing beyond 15 pawns. That's the same filter as Tessera's own data.
- **Output:** records go to `data/lichess/` in 10-million-position chunks of about
  680 MB, by default up to 100 million positions (`--max` to change). Each finished
  chunk is saved, so stopping loses at most the chunk in progress.
- **Training:** these positions have no game result, so the trainer learns them
  from the score alone; your own games keep the usual 80/20 mix of score and result.

Import speed was about 530,000 lines per second in testing (one thread), so
the whole file takes roughly 15 to 20 minutes.

## Step 3: put a better network into the engine

The engine ships with the network in `engine/net/tessera.nnue`. To replace it
with one you trained and measured to be stronger:

```bash
python3 tools/embed_net.py nets/tessera.nnue   # copy it to engine/net/ and regenerate engine/src/net_data.h
make -C engine && make -C engine wasm          # rebuild the native engine and both browser builds (needs clang for wasm)
git add engine/net engine/src/net_data.h engine/web && git commit -m "New network" && git push   # publish; GitHub rebuilds tessera.exe
```

On Windows without a compiler, run the first line with `python tools\embed_net.py nets\tessera.nnue`,
commit and push `engine/net` and `engine/src/net_data.h`; the browser builds then
have to be rebuilt on a machine with clang.

After a new network is in, newly generated games (step 1) are played with it,
so each round of generate, train, measure starts from a stronger player.
