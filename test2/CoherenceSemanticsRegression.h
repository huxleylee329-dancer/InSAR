#pragma once

// 相干性语义回归测试：验证 phase_coherence()（二倍角 R2）、
// phase_circular_concentration()（一阶圆统计 R1）与
// complex_coherence_demodulated()（去参考相位后的真复相干 gamma）三者的语义区分。
// 通过 test2.exe --coherence-semantics-regression 运行。
int RunCoherenceSemanticsRegression();
