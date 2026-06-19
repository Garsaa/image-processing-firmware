#if defined(__XTENSA__) || defined(__xtensa__)

  .section .iram1, "ax"
  .literal .LcaptureDueAddr, captureDue

  .global markCaptureDueFromTimerAsm
  .type markCaptureDueFromTimerAsm, @function

markCaptureDueFromTimerAsm:
  entry a1, 32
  l32r a8, .LcaptureDueAddr
  movi.n a9, 1
  s8i a9, a8, 0
  retw.n

  .size markCaptureDueFromTimerAsm, . - markCaptureDueFromTimerAsm

#endif
