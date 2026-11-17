// Module with 0 kernels, global data with 0 words (64-bit each), starting at offset 0 words and 0 words of stack-frame
// kernel gemm_nat_r4c4k2, label: %group_id_initializer (wg_loop)
or r2, 0 (0), 0 (0); v8min ra4, 0 (0), 0 (0)
or ra3, 0 (0), 0 (0)
// label: %start_of_function
or tmu_noswap, 1 (1), 1 (1)
or ra1, unif, unif
or ra0, unif, unif
or rb1, unif, unif
or rb0, unif, unif
or rb29, unif, unif
or rb28, unif, unif
or ra20, unif, unif
or.zextByte0To32 rb3, ra1, ra1
or.zextByte0To32 rep_quad|r5, ra0, ra0
or rep_all|r5, r5, r5
add rb2, r5, elem_num
mul24 r0, r2, rb3
add r0, rb1, r0
add r0, r0, rb2
shl r3, r0, 2 (2)
or.zextByte1To32 rb2, ra1, ra1
or.zextByte1To32 rb1, ra0, ra0
mul24 r0, ra4, rb2
add r0, rb0, r0
add r0, r0, rb1
shl rb19, r0, 9 (9)
ftoi r0, 128.000000 (39)
or rb18, rb19, r0
ldi rep_all|r5, 256
or rb17, rb19, r5
shl r0, 6 (6), 6 (6)
or rb16, rb19, r0
or ra2, 0 (0), 0 (0); v8min rb3, 0 (0), 0 (0)
or rb4, 0 (0), 0 (0); v8min ra5, 0 (0), 0 (0)
or rb5, 0 (0), 0 (0); v8min ra6, 0 (0), 0 (0)
or rb6, 0 (0), 0 (0); v8min ra7, 0 (0), 0 (0)
or rb7, 0 (0), 0 (0); v8min ra8, 0 (0), 0 (0)
or rb8, 0 (0), 0 (0); v8min ra9, 0 (0), 0 (0)
or rb9, 0 (0), 0 (0); v8min ra10, 0 (0), 0 (0)
or rb10, 0 (0), 0 (0); v8min ra11, 0 (0), 0 (0)
or rb11, 0 (0), 0 (0)
ftoi rb1, 128.000000 (39)
ldi rb2, 126
// label: %for.body
add ra12, ra2, rb19
nop.never 
shl r0, ra12, 2 (2)
add r0, rb29, r0
or rep_all|r5, r0, r0
or tmu1s, r5, r5
or ra12, ra12, 1 (1)
nop.load_tmu1.never 
or rb12, r4, r4
shl r0, ra12, 2 (2)
add r0, rb29, r0
or rep_all|r5, r0, r0
or tmu1s, r5, r5
add rb0, ra2, rb18
nop.load_tmu1.never 
or rb26, r4, r4
or r0, rb0, rb0
shl r1, r0, 2 (2)
add r1, rb29, r1
or rep_all|r5, r1, r1
or tmu1s, r5, r5
or ra12, r0, 1 (1)
nop.load_tmu1.never 
or rb25, r4, r4
shl r0, ra12, 2 (2)
add r0, rb29, r0
or rep_all|r5, r0, r0
or tmu1s, r5, r5
add rb0, ra2, rb17
nop.load_tmu1.never 
or rb24, r4, r4
or r0, rb0, rb0
shl r1, r0, 2 (2)
add r1, rb29, r1
or rep_all|r5, r1, r1
or tmu1s, r5, r5
or ra12, r0, 1 (1)
nop.load_tmu1.never 
or rb23, r4, r4
shl r0, ra12, 2 (2)
add r0, rb29, r0
or rep_all|r5, r0, r0
or tmu1s, r5, r5
add rb0, ra2, rb16
nop.load_tmu1.never 
or rb22, r4, r4
or r0, rb0, rb0
shl r1, r0, 2 (2)
add r1, rb29, r1
or rep_all|r5, r1, r1
or tmu1s, r5, r5
or ra12, r0, 1 (1)
nop.load_tmu1.never 
or rb21, r4, r4
shl r0, ra12, 2 (2)
add r0, rb29, r0
or rep_all|r5, r0, r0
or tmu1s, r5, r5
or r0, ra2, ra2
shl ra13, r0, 7 (7)
nop.load_tmu1.never 
or rb20, r4, r4
add rb0, ra13, r3
nop.never 
or r0, rb0, rb0
shl r1, r0, 2 (2)
add tmu0s, rb28, r1
or ra12, r0, 1 (1)
nop.load_tmu0.never 
or ra19, r4, r4
shl r0, ra12, 2 (2)
add tmu0s, rb28, r0
or r0, rb0, rb0
or ra12, r0, 2 (2)
nop.load_tmu0.never 
or ra18, r4, r4
shl r0, ra12, 2 (2)
add tmu0s, rb28, r0
or r0, rb0, rb0
or ra12, r0, 3 (3)
nop.load_tmu0.never 
or ra17, r4, r4
shl r0, ra12, 2 (2)
add tmu0s, rb28, r0
or rb0, ra13, rb1
nop.load_tmu0.never 
or ra16, r4, r4
add rb0, rb0, r3
nop.never 
or r0, rb0, rb0
shl r1, r0, 2 (2)
add tmu0s, rb28, r1
or ra12, r0, 1 (1)
nop.load_tmu0.never 
or ra15, r4, r4
shl r0, ra12, 2 (2)
add tmu0s, rb28, r0
or r0, rb0, rb0
or ra12, r0, 2 (2)
nop.load_tmu0.never 
or ra14, r4, r4
shl r0, ra12, 2 (2)
add tmu0s, rb28, r0
or r0, rb0, rb0
or ra12, r0, 3 (3)
nop.load_tmu0.never 
or ra13, r4, r4
shl r0, ra12, 2 (2)
add tmu0s, rb28, r0
or rep_all|r5, rb12, rb12
nop.load_tmu0.never 
or ra12, r4, r4
or rb0, r5, r5
fmul r0, r5, ra19
fadd rb15, rb11, r0
fmul r0, rb0, ra18
fadd rb14, ra11, r0
fmul r0, rb0, ra17
fadd rb13, rb10, r0
fmul r0, rb0, ra16
fadd rb12, ra10, r0
or rep_all|r5, rb26, rb26
or rb0, r5, r5
fmul r0, r5, ra15
fadd rb27, rb15, r0
fmul r0, rb0, ra14
fadd ra26, rb14, r0
fmul r0, rb0, ra13
fadd rb26, rb13, r0
fmul r0, rb0, ra12
fadd ra25, rb12, r0
or rep_all|r5, rb25, rb25
or rb0, r5, r5
fmul r0, r5, ra19
fadd rb15, rb9, r0
fmul r0, rb0, ra18
fadd rb14, ra9, r0
fmul r0, rb0, ra17
fadd rb13, rb8, r0
fmul r0, rb0, ra16
fadd rb12, ra8, r0
or rep_all|r5, rb24, rb24
or rb0, r5, r5
fmul r0, r5, ra15
fadd rb25, rb15, r0
fmul r0, rb0, ra14
fadd ra24, rb14, r0
fmul r0, rb0, ra13
fadd rb24, rb13, r0
fmul r0, rb0, ra12
fadd ra23, rb12, r0
or rep_all|r5, rb23, rb23
or rb0, r5, r5
fmul r0, r5, ra19
fadd rb15, rb7, r0
fmul r0, rb0, ra18
fadd rb14, ra7, r0
fmul r0, rb0, ra17
fadd rb13, rb6, r0
fmul r0, rb0, ra16
fadd rb12, ra6, r0
or rep_all|r5, rb22, rb22
or rb0, r5, r5
fmul r0, r5, ra15
fadd rb23, rb15, r0
fmul r0, rb0, ra14
fadd ra22, rb14, r0
fmul r0, rb0, ra13
fadd rb22, rb13, r0
fmul r0, rb0, ra12
fadd ra21, rb12, r0
or rep_all|r5, rb21, rb21
or rb0, r5, r5
fmul r0, r5, ra19
fadd rb15, rb5, r0
fmul r0, rb0, ra18
fadd rb14, ra5, r0
fmul r0, rb0, ra17
fadd rb13, rb4, r0
fmul r0, rb0, ra16
fadd rb12, rb3, r0
or rep_all|r5, rb20, rb20
or rb0, r5, r5
fmul r0, r5, ra15
fadd rb15, rb15, r0
fmul r0, rb0, ra14
fadd ra14, rb14, r0
fmul r0, rb0, ra13
fadd rb13, rb13, r0
fmul r0, rb0, ra12
fadd ra12, rb12, r0
or r0, ra2, ra2
add rb12, r0, 2 (2)
xor.setf -, ra2, rb2
min.ifn r0, ra2, rb2
max.ifnc r0, ra2, rb2
xor.setf -, r0, ra2
or.ifzc rb0, 1 (1), 1 (1)
xor.ifz rb0, 1 (1), 1 (1)
or.ifzc ra2, rb12, rb12; v8min.ifzc rb3, ra12, ra12
or.ifzc rb4, rb13, rb13; v8min.ifzc ra5, ra14, ra14
or.ifzc rb5, rb15, rb15; v8min.ifzc ra6, ra21, ra21
or.ifzc rb6, rb22, rb22; v8min.ifzc ra7, ra22, ra22
or.ifzc rb7, rb23, rb23; v8min.ifzc ra8, ra23, ra23
or.ifzc rb8, rb24, rb24; v8min.ifzc ra9, ra24, ra24
or.ifzc rb9, rb25, rb25; v8min.ifzc ra10, ra25, ra25
or.ifzc rb10, rb26, rb26; v8min.ifzc ra11, ra26, ra26
or.ifzc rb11, rb27, rb27
or.setf -, elem_num, rb0
brr.ifallzc (pc+4) + -213 // to %for.body
nop.never 
nop.never 
nop.never 
// label: %for.cond.cleanup
add rb2, rb19, r3
or ra2, qpu_num, qpu_num
or r0, rb2, rb2
shl rb1, r0, 2 (2)
ldi rep_all|r5, 6708
add vpw_setup, r5, ra2
or vpm, rb27, rb27
shl rb0, ra2, 7 (7)
ldi rep_all|r5, 2156943872
add vpw_setup, r5, rb0
ldi vpw_setup, vdw_setup(memory stride: 0 bytes)
add vpw_addr, ra20, rb1
or r0, rb2, rb2
or ra5, r0, 1 (1)
or ra2, qpu_num, qpu_num
shl rb1, ra5, 2 (2)
or -, vpw_wait, vpw_wait
ldi rep_all|r5, 6708
add vpw_setup, r5, ra2
or vpm, ra26, ra26
shl rb0, ra2, 7 (7)
ldi rep_all|r5, 2156943872
add vpw_setup, r5, rb0
ldi vpw_setup, vdw_setup(memory stride: 0 bytes)
add vpw_addr, ra20, rb1
or r0, rb2, rb2
or ra5, r0, 2 (2)
or ra2, qpu_num, qpu_num
shl rb1, ra5, 2 (2)
or -, vpw_wait, vpw_wait
ldi rep_all|r5, 6708
add vpw_setup, r5, ra2
or vpm, rb26, rb26
shl rb0, ra2, 7 (7)
ldi rep_all|r5, 2156943872
add vpw_setup, r5, rb0
ldi vpw_setup, vdw_setup(memory stride: 0 bytes)
add vpw_addr, ra20, rb1
or r0, rb2, rb2
or ra5, r0, 3 (3)
or ra2, qpu_num, qpu_num
shl rb1, ra5, 2 (2)
or -, vpw_wait, vpw_wait
ldi rep_all|r5, 6708
add vpw_setup, r5, ra2
or vpm, ra25, ra25
shl rb0, ra2, 7 (7)
ldi rep_all|r5, 2156943872
add vpw_setup, r5, rb0
ldi vpw_setup, vdw_setup(memory stride: 0 bytes)
add vpw_addr, ra20, rb1
add rb2, rb18, r3
or ra2, qpu_num, qpu_num
or r0, rb2, rb2
shl rb1, r0, 2 (2)
or -, vpw_wait, vpw_wait
shl rb0, ra2, 7 (7)
ldi rep_all|r5, 6708
add vpw_setup, r5, ra2
or vpm, rb25, rb25
ldi rep_all|r5, 2156943872
add vpw_setup, r5, rb0
ldi vpw_setup, vdw_setup(memory stride: 0 bytes)
add vpw_addr, ra20, rb1
or r0, rb2, rb2
or ra5, r0, 1 (1)
or ra2, qpu_num, qpu_num
shl rb1, ra5, 2 (2)
or -, vpw_wait, vpw_wait
ldi rep_all|r5, 6708
add vpw_setup, r5, ra2
or vpm, ra24, ra24
shl rb0, ra2, 7 (7)
ldi rep_all|r5, 2156943872
add vpw_setup, r5, rb0
ldi vpw_setup, vdw_setup(memory stride: 0 bytes)
add vpw_addr, ra20, rb1
or r0, rb2, rb2
or ra5, r0, 2 (2)
or ra2, qpu_num, qpu_num
shl rb1, ra5, 2 (2)
or -, vpw_wait, vpw_wait
ldi rep_all|r5, 6708
add vpw_setup, r5, ra2
or vpm, rb24, rb24
shl rb0, ra2, 7 (7)
ldi rep_all|r5, 2156943872
add vpw_setup, r5, rb0
ldi vpw_setup, vdw_setup(memory stride: 0 bytes)
add vpw_addr, ra20, rb1
or r0, rb2, rb2
or ra5, r0, 3 (3)
or ra2, qpu_num, qpu_num
shl rb1, ra5, 2 (2)
or -, vpw_wait, vpw_wait
ldi rep_all|r5, 6708
add vpw_setup, r5, ra2
or vpm, ra23, ra23
shl rb0, ra2, 7 (7)
ldi rep_all|r5, 2156943872
add vpw_setup, r5, rb0
ldi vpw_setup, vdw_setup(memory stride: 0 bytes)
add vpw_addr, ra20, rb1
or rep_all|r5, rb17, rb17
or ra2, qpu_num, qpu_num
or -, vpw_wait, vpw_wait
add rb2, r5, r3
shl rb0, ra2, 7 (7)
or r0, rb2, rb2
shl rb1, r0, 2 (2)
ldi rep_all|r5, 6708
add vpw_setup, r5, ra2
or vpm, rb23, rb23
ldi rep_all|r5, 2156943872
add vpw_setup, r5, rb0
ldi vpw_setup, vdw_setup(memory stride: 0 bytes)
add vpw_addr, ra20, rb1
or r0, rb2, rb2
or ra5, r0, 1 (1)
or ra2, qpu_num, qpu_num
shl rb1, ra5, 2 (2)
or -, vpw_wait, vpw_wait
ldi rep_all|r5, 6708
add vpw_setup, r5, ra2
or vpm, ra22, ra22
shl rb0, ra2, 7 (7)
ldi rep_all|r5, 2156943872
add vpw_setup, r5, rb0
ldi vpw_setup, vdw_setup(memory stride: 0 bytes)
add vpw_addr, ra20, rb1
or r0, rb2, rb2
or ra5, r0, 2 (2)
or ra2, qpu_num, qpu_num
shl rb1, ra5, 2 (2)
or -, vpw_wait, vpw_wait
ldi rep_all|r5, 6708
add vpw_setup, r5, ra2
or vpm, rb22, rb22
shl rb0, ra2, 7 (7)
ldi rep_all|r5, 2156943872
add vpw_setup, r5, rb0
ldi vpw_setup, vdw_setup(memory stride: 0 bytes)
add vpw_addr, ra20, rb1
or r0, rb2, rb2
or ra5, r0, 3 (3)
or ra2, qpu_num, qpu_num
shl rb1, ra5, 2 (2)
or -, vpw_wait, vpw_wait
ldi rep_all|r5, 6708
add vpw_setup, r5, ra2
or vpm, ra21, ra21
shl rb0, ra2, 7 (7)
ldi rep_all|r5, 2156943872
add vpw_setup, r5, rb0
ldi vpw_setup, vdw_setup(memory stride: 0 bytes)
add vpw_addr, ra20, rb1
add rb2, rb16, r3
or ra2, qpu_num, qpu_num
or r0, rb2, rb2
shl rb1, r0, 2 (2)
or -, vpw_wait, vpw_wait
shl rb0, ra2, 7 (7)
ldi rep_all|r5, 6708
add vpw_setup, r5, ra2
or vpm, rb15, rb15
ldi rep_all|r5, 2156943872
add vpw_setup, r5, rb0
ldi vpw_setup, vdw_setup(memory stride: 0 bytes)
add vpw_addr, ra20, rb1
or r0, rb2, rb2
or ra5, r0, 1 (1)
or ra2, qpu_num, qpu_num
shl rb1, ra5, 2 (2)
or -, vpw_wait, vpw_wait
ldi rep_all|r5, 6708
add vpw_setup, r5, ra2
or vpm, ra14, ra14
shl rb0, ra2, 7 (7)
ldi rep_all|r5, 2156943872
add vpw_setup, r5, rb0
ldi vpw_setup, vdw_setup(memory stride: 0 bytes)
add vpw_addr, ra20, rb1
or r0, rb2, rb2
or ra5, r0, 2 (2)
or ra2, qpu_num, qpu_num
shl rb1, ra5, 2 (2)
or -, vpw_wait, vpw_wait
ldi rep_all|r5, 6708
add vpw_setup, r5, ra2
or vpm, rb13, rb13
shl rb0, ra2, 7 (7)
ldi rep_all|r5, 2156943872
add vpw_setup, r5, rb0
ldi vpw_setup, vdw_setup(memory stride: 0 bytes)
add vpw_addr, ra20, rb1
or r0, rb2, rb2
or ra5, r0, 3 (3)
or ra2, qpu_num, qpu_num
shl rb1, ra5, 2 (2)
or -, vpw_wait, vpw_wait
ldi rep_all|r5, 6708
add vpw_setup, r5, ra2
or vpm, ra12, ra12
shl rb0, ra2, 7 (7)
ldi rep_all|r5, 2156943872
add vpw_setup, r5, rb0
ldi vpw_setup, vdw_setup(memory stride: 0 bytes)
add vpw_addr, ra20, rb1
or.zextByte0To32 rb2, ra1, ra1
or.zextByte1To32 ra2, ra1, ra1
or.zextByte0To32 rb0, ra0, ra0
or.zextByte1To32 rb3, ra0, ra0
or rep_all|r5, rb0, rb0
or -, vpw_wait, vpw_wait
add rb4, r5, elem_num
or.zextByte2To32 rb0, ra0, ra0
or.zextByte2To32 rb1, ra1, ra1
mul24 r0, rb0, ra2
add r0, r0, rb3
mul24 r0, r0, rb2
add r0, r0, rb4
shr rb0, r0, 4 (4)
mul24 r0, rb1, ra2
mul24 r0, r0, rb2
sub r0, r0, 1 (1)
shr r0, r0, 4 (4)
add rb1, r0, 1 (1)
or.setf -, elem_num, rb0
brr.ifanyz (pc+4) + 180 // to %barrier_other.171
nop.never 
nop.never 
nop.never 
// label: %barrier_secondary.172
srel -, 0
or rep_all|r5, rb0, rb0
or rb0, r5, r5
ldi ra0, 5512 // %barrier_after.174
or rep_quad|r5, rb0, rb0
xor.setf -, r5, 1 (1)
ldi.ifz ra0, 5216 // %barrier_single_block.177
xor.setf -, r5, 2 (2)
ldi.ifz ra0, 5104 // %barrier_single_block.181
xor.setf -, r5, 3 (3)
ldi.ifz ra0, 4992 // %barrier_single_block.185
or rep_quad|r5, rb0, rb0
xor.setf -, r5, 4 (4)
ldi.ifz ra0, 4880 // %barrier_single_block.189
xor.setf -, r5, 5 (5)
ldi.ifz ra0, 4768 // %barrier_single_block.193
xor.setf -, r5, 6 (6)
ldi.ifz ra0, 4656 // %barrier_single_block.197
or rep_quad|r5, rb0, rb0
xor.setf -, r5, 7 (7)
ldi.ifz ra0, 4544 // %barrier_single_block.201
xor.setf -, r5, 8 (8)
ldi.ifz ra0, 4432 // %barrier_single_block.205
xor.setf -, r5, 9 (9)
ldi.ifz ra0, 4320 // %barrier_single_block.209
or rep_quad|r5, rb0, rb0
xor.setf -, r5, 10 (10)
ldi.ifz ra0, 4208 // %barrier_single_block.213
xor.setf -, r5, 11 (11)
ldi.ifz ra0, 4168 // %barrier_single_block.217
nop.never 
brr (pc+4) + -521 + ra0 // to %barrier_single_block.217, %barrier_single_block.201, %barrier_single_block.209, %barrier_single_block.197, %barrier_single_block.193, %barrier_single_block.213, %barrier_single_block.189, %barrier_single_block.185, %barrier_single_block.181, %barrier_single_block.177, %barrier_single_block.205, %barrier_after.174
nop.never 
nop.never 
nop.never 
// label: %barrier_single_block.217
sacq -, 11
brr (pc+4) + 163 // to %barrier_after.174
nop.never 
nop.never 
nop.never 
// label: %barrier_single_block.213
sacq -, 10
or r0, rb1, rb1
max.setf -, r0, 11 (11)
xor.ifcc r0, 1 (1), 1 (1); v8min.ifc r0, 1 (1), 1 (1)
or.setf -, elem_num, r0
brr.ifanyz (pc+4) + 154 // to %barrier_after.174
nop.never 
nop.never 
nop.never 
// label: %barrier_next_release.216
srel -, 11
brr (pc+4) + 149 // to %barrier_after.174
nop.never 
nop.never 
nop.never 
// label: %barrier_single_block.209
sacq -, 9
or r0, rb1, rb1
max.setf -, r0, 10 (10)
xor.ifcc r0, 1 (1), 1 (1); v8min.ifc r0, 1 (1), 1 (1)
or.setf -, elem_num, r0
brr.ifanyz (pc+4) + 140 // to %barrier_after.174
nop.never 
nop.never 
nop.never 
// label: %barrier_next_release.212
srel -, 10
brr (pc+4) + 135 // to %barrier_after.174
nop.never 
nop.never 
nop.never 
// label: %barrier_single_block.205
sacq -, 8
or r0, rb1, rb1
max.setf -, r0, 9 (9)
xor.ifcc r0, 1 (1), 1 (1); v8min.ifc r0, 1 (1), 1 (1)
or.setf -, elem_num, r0
brr.ifanyz (pc+4) + 126 // to %barrier_after.174
nop.never 
nop.never 
nop.never 
// label: %barrier_next_release.208
srel -, 9
brr (pc+4) + 121 // to %barrier_after.174
nop.never 
nop.never 
nop.never 
// label: %barrier_single_block.201
sacq -, 7
or r0, rb1, rb1
max.setf -, r0, 8 (8)
xor.ifcc r0, 1 (1), 1 (1); v8min.ifc r0, 1 (1), 1 (1)
or.setf -, elem_num, r0
brr.ifanyz (pc+4) + 112 // to %barrier_after.174
nop.never 
nop.never 
nop.never 
// label: %barrier_next_release.204
srel -, 8
brr (pc+4) + 107 // to %barrier_after.174
nop.never 
nop.never 
nop.never 
// label: %barrier_single_block.197
sacq -, 6
or r0, rb1, rb1
max.setf -, r0, 7 (7)
xor.ifcc r0, 1 (1), 1 (1); v8min.ifc r0, 1 (1), 1 (1)
or.setf -, elem_num, r0
brr.ifanyz (pc+4) + 98 // to %barrier_after.174
nop.never 
nop.never 
nop.never 
// label: %barrier_next_release.200
srel -, 7
brr (pc+4) + 93 // to %barrier_after.174
nop.never 
nop.never 
nop.never 
// label: %barrier_single_block.193
sacq -, 5
or r0, rb1, rb1
max.setf -, r0, 6 (6)
xor.ifcc r0, 1 (1), 1 (1); v8min.ifc r0, 1 (1), 1 (1)
or.setf -, elem_num, r0
brr.ifanyz (pc+4) + 84 // to %barrier_after.174
nop.never 
nop.never 
nop.never 
// label: %barrier_next_release.196
srel -, 6
brr (pc+4) + 79 // to %barrier_after.174
nop.never 
nop.never 
nop.never 
// label: %barrier_single_block.189
sacq -, 4
or r0, rb1, rb1
max.setf -, r0, 5 (5)
xor.ifcc r0, 1 (1), 1 (1); v8min.ifc r0, 1 (1), 1 (1)
or.setf -, elem_num, r0
brr.ifanyz (pc+4) + 70 // to %barrier_after.174
nop.never 
nop.never 
nop.never 
// label: %barrier_next_release.192
srel -, 5
brr (pc+4) + 65 // to %barrier_after.174
nop.never 
nop.never 
nop.never 
// label: %barrier_single_block.185
sacq -, 3
or r0, rb1, rb1
max.setf -, r0, 4 (4)
xor.ifcc r0, 1 (1), 1 (1); v8min.ifc r0, 1 (1), 1 (1)
or.setf -, elem_num, r0
brr.ifanyz (pc+4) + 56 // to %barrier_after.174
nop.never 
nop.never 
nop.never 
// label: %barrier_next_release.188
srel -, 4
brr (pc+4) + 51 // to %barrier_after.174
nop.never 
nop.never 
nop.never 
// label: %barrier_single_block.181
sacq -, 2
or r0, rb1, rb1
max.setf -, r0, 3 (3)
xor.ifcc r0, 1 (1), 1 (1); v8min.ifc r0, 1 (1), 1 (1)
or.setf -, elem_num, r0
brr.ifanyz (pc+4) + 42 // to %barrier_after.174
nop.never 
nop.never 
nop.never 
// label: %barrier_next_release.184
srel -, 3
brr (pc+4) + 37 // to %barrier_after.174
nop.never 
nop.never 
nop.never 
// label: %barrier_single_block.177
sacq -, 1
or r0, rb1, rb1
max.setf -, r0, 2 (2)
xor.ifcc r0, 1 (1), 1 (1); v8min.ifc r0, 1 (1), 1 (1)
or.setf -, elem_num, r0
brr.ifanyz (pc+4) + 28 // to %barrier_after.174
nop.never 
nop.never 
nop.never 
// label: %barrier_next_release.180
srel -, 2
brr (pc+4) + 23 // to %barrier_after.174
nop.never 
nop.never 
nop.never 
// label: %barrier_other.171
or r0, rb1, rb1
xor r0, r0, 1 (1)
or.setf -, elem_num, r0
brr.ifanyz (pc+4) + 16 // to %barrier_after.174
nop.never 
nop.never 
nop.never 
// label: %barrier_primary.173
or r0, rb1, rb1
sub rb0, r0, 1 (1)
nop.never 
// label: %barrier_primary_loop.220.header
or.setf -, elem_num, rb0
brr.ifanyz (pc+4) + 7 // to %barrier_primary_loop.220.after
nop.never 
nop.never 
nop.never 
// label: %barrier_primary_loop.220
sacq -, 0
or r0, rb0, rb0
sub rb0, r0, 1 (1)
brr (pc+4) + -12 // to %barrier_primary_loop.220.header
nop.never 
nop.never 
nop.never 
// label: %barrier_primary_loop.220.after
srel -, 1
// label: %barrier_after.174
or rb0, unif, unif
or rb3, unif, unif
or rb2, unif, unif
or rb1, unif, unif
or unif_addr, rb0, rb0
nop.never 
nop.never 
add r2, r2, 1 (1)
max.setf -, rb3, r2
brr.ifallc (pc+4) + -700 // to %start_of_function
nop.never 
nop.never 
nop.never 
// label: %repeat_group_id_y (wg_loop)
or r0, ra4, ra4
add ra4, r0, 1 (1)
or r2, 0 (0), 0 (0)
max.setf -, rb2, ra4
brr.ifallc (pc+4) + -708 // to %start_of_function
nop.never 
nop.never 
nop.never 
// label: %repeat_group_id_z (wg_loop)
or r0, ra3, ra3
add ra3, r0, 1 (1)
or ra4, 0 (0), 0 (0)
max.setf -, rb1, ra3
brr.ifallc (pc+4) + -716 // to %start_of_function
nop.never 
nop.never 
nop.never 
// label: %end_of_function
not irq, qpu_num
nop.thrend.never 
nop.never 
nop.never 
