set -u
mkdir -p results
RUNS=3

echo "Інформація про систему" | tee results/system.txt
{
  sw_vers
  gcc --version 2>&1 | head -2
  sysctl machdep.cpu.brand_string hw.memsize hw.pagesize hw.cachelinesize
  sysctl hw.perflevel0.name hw.perflevel0.physicalcpu hw.perflevel0.l1dcachesize hw.perflevel0.l2cachesize
  sysctl hw.perflevel1.name hw.perflevel1.physicalcpu hw.perflevel1.l1dcachesize hw.perflevel1.l2cachesize
} 2>&1 | tee -a results/system.txt

echo "Перевірка векторизації (для п. про i-j-k та i-k-j)" | tee results/vectorization.txt
for f in matrix.c memory_access.c; do
  echo "--- $f ---" >> results/vectorization.txt
  gcc -O2 -std=c11 -D_DARWIN_C_SOURCE -Rpass=loop-vectorize -Rpass-missed=loop-vectorize -c "$f" -o /dev/null >> results/vectorization.txt 2>&1 \
  || gcc -O2 -std=c11 -D_DARWIN_C_SOURCE -fopt-info-vec-all -c "$f" -o /dev/null >> results/vectorization.txt 2>&1
done

for i in $(seq 1 $RUNS); do
  echo "Запуск $i з $RUNS: memory_access";  ./memory_access all 128 | tee results/memory_access_$i.txt
  echo "Запуск $i з $RUNS: matrix 512";     ./matrix 512            | tee results/matrix512_$i.txt
  echo "Запуск $i з $RUNS: matrix 1024";    ./matrix 1024           | tee results/matrix1024_$i.txt
  echo "Запуск $i з $RUNS: threads";        ./threads all           | tee results/threads_$i.txt
  echo "Запуск $i з $RUNS: padding 64 Б";   ./threads_pad64 2       | tee results/threads_pad64_$i.txt
  echo "Запуск $i з $RUNS: padding 256 Б";  ./threads_pad256 2      | tee results/threads_pad256_$i.txt
done