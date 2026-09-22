# SimpleLogics NNUE V1

## Tujuan

SimpleLogics sekarang dikembangkan dengan perubahan besar pada
komponen evaluasi: membuat NNUE yang dilatih khusus untuk SimpleLogics.

Search tetap menggunakan Stockfish sebagai fondasi search,
tetapi network evaluasinya akan menjadi network SimpleLogics.

Architecture:

Stockfish Search
        |
        v
SimpleLogics NNUE
        |
        v
   Best Move
        |
        v
     DroidFish

---

## Kenapa NNUE?

Percobaan sebelumnya:

- Recapture extension
- Quiet extension
- LMR adjustment
- Search adjustment

menghasilkan keputusan yang hampir sama pada benchmark.

Karena itu SimpleLogics tidak lagi difokuskan pada perubahan kecil
di search.

Perubahan berikutnya difokuskan pada evaluation network.

---

## Training

SimpleLogics menggunakan trainer NNUE berbasis PyTorch.

Repository trainer resmi:

https://github.com/official-stockfish/nnue-pytorch

Dataset training harus menggunakan format yang kompatibel dengan
trainer NNUE.

Format utama:

.binpack

Network harus menggunakan feature architecture yang kompatibel
dengan engine.

Feature awal:

HalfKAv2_hm^

---

## Training Dataset

Dataset harus berisi posisi catur dalam jumlah besar.

Sumber data dapat berasal dari:

1. game engine
2. game manusia
3. posisi yang dihasilkan engine
4. posisi middlegame
5. posisi endgame
6. posisi tactical
7. posisi opening

Dataset harus memiliki distribusi posisi yang cukup luas.

Jangan melatih network hanya dari opening atau posisi tactical.

---

## Teacher

Untuk tahap pertama, posisi dapat diberi label menggunakan
engine yang kuat.

Contoh:

Stockfish

Teacher menghasilkan evaluasi posisi yang kemudian digunakan
untuk melatih SimpleLogics NNUE.

Tujuannya bukan sekadar menyalin satu posisi tertentu,
tetapi mempelajari pola evaluasi dari dataset yang besar.

---

## Training

Training membutuhkan komputer dengan GPU.

Contoh instalasi:

git clone --depth 1 https://github.com/official-stockfish/nnue-pytorch.git

cd nnue-pytorch

pip install -r requirements.txt

---

## Dataset

Contoh:

train.binpack
validation.binpack

Training dataset digunakan untuk pembelajaran.

Validation dataset digunakan untuk melihat apakah network
benar-benar belajar dan tidak hanya menghafal training data.

---

## Target

Tahap pertama:

SimpleLogics NNUE V1

Target:

- kompatibel dengan Stockfish NNUE
- dapat dimuat oleh engine
- dapat digunakan di DroidFish
- tidak menyebabkan engine crash
- evaluasi berbeda dari network bawaan
- dapat diuji engine-vs-engine

---

## Pengujian

Network tidak dianggap lebih kuat hanya karena training selesai.

Setelah network selesai:

1. compile SimpleLogics
2. load SimpleLogics NNUE
3. jalankan benchmark
4. jalankan engine-vs-engine
5. minimal 100 game untuk pemeriksaan awal
6. kemudian lakukan test lebih besar

Hasil yang dicatat:

SimpleLogics wins
Stockfish wins
Draws
Score
Average game length

---

## Penting

Jangan mengganti network bawaan engine dengan file NNUE
yang belum diverifikasi kompatibilitasnya.

Network harus:

- memiliki architecture yang sesuai
- memiliki feature set yang sesuai
- memiliki format serialization yang sesuai
- lolos pemeriksaan network engine

---

## Roadmap

### V1

Train SimpleLogics NNUE.

### V2

Perbaiki dataset dan training.

### V3

Fine-tuning network menggunakan posisi yang lebih sulit.

### V4

Engine strength testing.

### V5

Optimization untuk Android ARM64.

---

## Prinsip pengembangan

SimpleLogics tidak akan dibuat lebih kuat dengan menambahkan
puluhan tweak kecil tanpa pengukuran.

Perubahan utama harus memiliki efek yang dapat diukur.

Search:

Stockfish foundation

Evaluation:

SimpleLogics NNUE

Interface:

UCI

Platform:

Android ARM64 / DroidFish
