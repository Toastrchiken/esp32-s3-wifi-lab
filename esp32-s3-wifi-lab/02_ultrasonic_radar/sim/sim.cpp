// Software-in-the-loop test: runs the real firmware (src/main.cpp) on a PC against
// a simulated room and HC-SR04, then checks learning, false alarms and detection.
//   g++ -std=gnu++17 -I sim/mocks -I src -DESP_ARDUINO_VERSION_MAJOR=2 sim/sim.cpp -o sim_test && ./sim_test
#include <random>
uint32_t g_ms = 0, g_duty = 0; bool g_intruder = false; double g_iang = 120, g_icm = 100; std::mt19937 rng(42);
#include "main.cpp"
// simulated room: back wall 300 cm, cupboard 150 cm at 60-80 deg, open doorway (no echo) at 160-180
unsigned long pulseIn(int, int, unsigned long timeout) {
  double us_pulse = g_duty * 20000.0 / ((1u<<LEDC_BITS)-1);
  double ang = (us_pulse - SERVO_MIN_US) * 180.0 / (SERVO_MAX_US - SERVO_MIN_US);
  double cm = 300;
  if (ang >= 60 && ang <= 80) cm = 150;
  if (ang >= 160) { g_ms += timeout/1000; return 0; }
  if (g_intruder && fabs(ang - g_iang) <= 7) cm = g_icm;       // person ~15 deg wide at 1 m
  std::normal_distribution<double> n(0, 3); cm += n(rng);
  if (std::uniform_real_distribution<double>(0,1)(rng) < 0.03) { g_ms += timeout/1000; return 0; } // 3% dropouts
  double c = 331.3 + 0.606 * tempC;
  unsigned long us = (unsigned long)(cm * 20000.0 / c);
  g_ms += us / 1000;
  return us;
}
void runFor(uint32_t ms) { uint32_t end = g_ms + ms; while (g_ms < end) { loop(); g_ms += 1; } }
int main() {
  setup();
  startLearning();
  uint32_t t0 = g_ms;
  while (learning && g_ms - t0 < 120000) { loop(); g_ms += 1; }
  ::printf("learned=%d after %.1f s\n", learned, (g_ms - t0) / 1000.0);
  ::printf("baseline: 0=%.0f 70=%.0f 120=%.0f 165=%.0f 180=%.0f\n", baseline[0], baseline[70], baseline[120], baseline[165], baseline[180]);
  startArmed();
  runFor(30000);
  ::printf("armed 30 s, empty room: alarms=%lu  alarm=%d\n", (unsigned long)alarmCount, alarmOn);
  g_intruder = true;
  runFor(30000);
  ::printf("person at 120 deg for 30 s: events=%lu\n", (unsigned long)evSeq);
  g_intruder = false; alarmOn = false;   // user pressed ACK
  runFor(20000);
  ::printf("room empty again 20 s: events=%lu alarm=%d\n", (unsigned long)evSeq, alarmOn);
  g_intruder = true; g_iang = 40; g_icm = 180;
  runFor(15000);
  ::printf("intruder 15 s: alarms=%lu alarm=%d  events=%lu\n", (unsigned long)alarmCount, alarmOn, (unsigned long)evSeq);
  for (uint32_t s = 1; s <= evSeq; s++) { Event& e = evs[s % NEV]; ::printf("  event %lu: %d deg  %d cm  base %d\n", (unsigned long)e.seq, e.angle, e.dist, e.base); }
  int flagged = 0; for (int i = 0; i < NPTS; i++) if (pts[i].flag == 2) flagged++;
  ::printf("confirmed points in ring: %d\n", flagged);
}
