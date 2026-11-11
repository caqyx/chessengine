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

## Step 2: train the network

Not written yet. It will be a second script with the same stop-and-resume
behaviour (it saves a checkpoint after every pass over the data), running on
the graphics card. It reads `data/local/` plus the files in `data/`.
