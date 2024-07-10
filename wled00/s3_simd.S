.text
.align 4
.global s3_mul16x16
.type   s3_mul16x16,@function
s3_mul16x16:
  # a2 = buffer
  # a3 = bright
  # a4 = loops
  entry          a1,16              # prepare windowed registers and reserve 16 bytes of stack
  movi.n         a8, 8              # set 8 for our right-shift to load into SAR
  wsr.sar        a8                 # set the numbers of bits to bits to right-shift from a8
  ee.vld.128.ip  q1,a3,0            # load 16 "B" values into Q1 from A3, don't move the pointer because this is static
  movi.n         a5, 0              # start our loop counter at 0
  loop:
    ee.vld.128.ip  q0,a2,0          # load 16 "A" values into Q0 from A2, don't move pointer because we're gonna write back to this
    ee.vmul.u8     q2,q0,q1         # C = A*B then >> by wsr.sar
    ee.vst.128.ip  q2,a2,16         # store the 16 "C" values back into a2, then move the pointer by 16 to get a new batch
    beq            a5, a4, exit     # branch to "exit:" if a4 == a5 (our number of 16-unit loops)
    addi           a5, a5, 1        # increment a5, our loop counter
    j              loop             # jump to "loop:"
  exit:
    retw.n                          # restore state (windowed registers) and return to caller
 