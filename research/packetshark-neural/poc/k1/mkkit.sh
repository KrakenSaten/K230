#!/bin/bash
# Builds the K1 transfer kit for Unit A: copies the inputs under short names,
# strips a copy of infer-bench, writes SHA256SUMS, gzip+base64 of every file
# and sendkit.ps1 (one sendfile.ps1 call per file). Run in WSL.
set -euo pipefail
A=/mnt/c/K230-artifacts/packetshark-neural
K=$A/k1-unit-a/kit
S=$A/k1-unit-a/scripts
SDK=$HOME/work/t-display-k230/k230_linux_sdk/output/k230_pocketos_defconfig
P=$HOME/work/psk-neural/poc-build
R=$HOME/work/psk-neural/runs
cd "$K"
cp "$P/infer-bench" infer-bench.full
"$SDK/host/bin/riscv64-unknown-linux-gnu-strip" -o infer-bench infer-bench.full
cp "$R/e2/dna_b64__u8/model.kmodel" u8.kmodel
cp "$R/e2/dna_b64__a16w8/model.kmodel" a16.kmodel
cp "$R/e3b/dna_b64.weights.f32" w.f32
cp "$R/e3b/dna_b64.test_in.f32" in.f32
cp "$R/e3b/dna_b64.float_out.f32" float.f32
cp "$R/e2/dna_b64__u8.sim_out.f32" sim_u8.f32
cp "$R/e2/dna_b64__a16w8.sim_out.f32" sim_a16.f32
FILES="k1.sh infer-bench u8.kmodel a16.kmodel w.f32 in.f32 float.f32 sim_u8.f32 sim_a16.f32"
sha256sum $FILES | sed 's#  #  /tmp/k1/#' > SHA256SUMS
sha256sum infer-bench.full >> SHA256SUMS.host
{
	echo '$ErrorActionPreference = "Stop"'
	echo '$S = "C:\K230-artifacts\vision-stream\scripts\sendfile.ps1"; $K = "C:\K230-artifacts\packetshark-neural\k1-unit-a\kit"'
	for f in $FILES SHA256SUMS; do
		gzip -9 -n -c "$f" > "$f.gz"
		base64 -w 76 "$f.gz" > "$f.b64"
		gs=$(sha256sum "$f.gz" | cut -d' ' -f1)
		bs=$(sha256sum "$f" | cut -d' ' -f1)
		echo "& \$S -B64 \"\$K\\$f.b64\" -Remote /tmp/k1/$f -GzSha $gs -BinSha $bs"
		rm "$f.gz"
	done
} > "$S/sendkit.ps1"
ls -l
cat SHA256SUMS
du -cb ./*.b64 | tail -1
