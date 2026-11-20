/* The current stage, printed once when it changes and kept for the crash report. */
#ifndef SOTB_STARTUP_PHASE_H
#define SOTB_STARTUP_PHASE_H

#ifdef __cplusplus
extern "C" {
#endif

/* `phase` must outlive the process. The first call with a new text prints it once. */
void sotb_startup_phase(const char *phase);
const char *sotb_startup_phase_current(void);

/* For a milestone reached from a per-frame path: only the first call for `reached` changes the stage. */
void sotb_startup_phase_once(int *reached, const char *phase);

#ifdef __cplusplus
}
#endif

#endif
