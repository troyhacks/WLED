.text
.align 4
.global p4_mul16x16
.type   p4_mul16x16,@function
# Needs -march rv32imafc_zicsr_zifencei_xesppie -mabi ilp32f
p4_mul16x16:
  # p4_mul16x16(uint8_t *buffer, uint8_t *bright, uint16_t loops)
  # a0 = buffer, a1 = brightness, a2 = num_loops
  add               sp, sp, -32     #
  # Enable aligned data access
  esp.movx.r.cfg    t6            
  or                t6, t6, 2
  esp.movx.w.cfg    t6
  li                t6, 8           # put 8 in temp register 6
  esp.movx.w.sar    t6              # set the numbers of bits to right-shift from t6
  esp.vld.128.ip    q1, a1, 0       # load 16 "B" values into Q1 from A1, don't move the pointer because this is static
  li                t1, 0           # start our loop counter t1 at 0
  loop:                             # "loop" label
    beq             t1, a2, exit    # branch to "exit" if loop_num == num_loops 
    esp.vld.128.ip  q0, a0, 0       # load 16 "A" values into Q0 from A0, don't move pointer because we're gonna write back to this
    esp.vmul.u8     q2, q0, q1      # C = A*B then >> by esp.movx.w.sar
    esp.vst.128.ip  q2, a0, 16      # store the 16 "C" values back into a0, then move the pointer by 16 to get a new batch
    addi            t1, t1, 1       # increment loop counter t1
    j               loop            # jump to "loop"
  exit:                             # "exit" label
    add             sp, sp, 32      #
    ret                             # return
