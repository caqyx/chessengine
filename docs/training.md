# Training on your own computer

The goal is a neural-network evaluation trained only on Tessera's own games.
That needs about 100 million positions. The repository holds 8 million; the
rest is generated on your machine with the script below. It can be stopped and
continued as often as you like.

## Step 1: generate games (stop and resume freely)

### Windows

The engine needs a GCC or Clang compiler, which is simplest to get through WSL
(Ubuntu running inside Windows).

```powershell
wsl --install -d Ubuntu     # one time, in PowerShell as administrator; reboot if it asks, then open "Ubuntu" from the Start menu
```

Then, inside the Ubuntu window:

```bash
sudo apt update && sudo apt install -y g++ make python3 git   # compiler, build tool, Python, git
cd ~                                                          # stay in the Linux home folder: /mnt/c is several times slower
git clone https://github.com/caqyx/chessengine.git            # download the project
cd chessengine
make -C engine                                                # build the engine, optimised for your CPU
python3 tools/gen_data.py                                     # start generating
```

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
git pull && make -C engine                # later: get a newer engine before continuing (old data stays valid)
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
