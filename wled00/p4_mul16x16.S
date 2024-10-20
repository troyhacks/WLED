.text
.align 4
.global p4_mul16x16
.type   p4_mul16x16,@function
# ESP32-P4 needs -march rv32imafc_zicsr_zifencei_xesppie -mabi ilp32f
# a0 = out_packet, a1 = brightness, a2 = num_loops, a3 = pixelbuffer
p4_mul16x16:
  esp.movx.r.cfg    t6              # Enable aligned data access
  or                t6, t6, 2       # Enable aligned data access
  esp.movx.w.cfg    t6              # Enable aligned data access
  li                t6, 8           # put 8 in temp register 6
  esp.movx.w.sar    t6              # set the numbers of bits to right-shift from t6
  esp.vldbc.8.ip    q1, a1, 0       # load the "B" value into q1 from a1, broadcasting the same value to all 16 values of q1
  li                t1, 0           # start our loop_num counter t1 at 0
  loop:                             # "loop" label
    beq             t1, a2, exit    # branch to "exit" if loop_num == num_loops 
    esp.vld.128.ip  q0, a3, 16      # load 16 "A" values into q0 from a3, then move the pointer by 16 to get a new batch
    esp.vmul.u8     q2, q0, q1      # C = A*B (q2 = q0 * q1) then >> by esp.movx.w.sar which we set to 8
    esp.vst.128.ip  q2, a0, 16      # store the 16 "C" values into a0, then move the pointer by 16
    addi            t1, t1, 1       # increment loop_num counter t1
    j               loop            # jump to "loop"
  exit:                             # "exit" label
    ret                             # return
