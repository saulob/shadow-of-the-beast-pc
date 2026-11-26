#ifndef SOTB_SIMULATE_CRASH_H
#define SOTB_SIMULATE_CRASH_H

#ifdef __cplusplus
extern "C" {
#endif

/* Development only: SOTB_SIMULATE_CRASH=1 and nothing else arms the simulated crash. */
int sotb_simulate_crash_requested(const char *value);

/* Windows: a real access violation when SOTB_SIMULATE_CRASH=1, through whatever fault handling is installed. Other values
 * do nothing; Linux says it is unsupported. Call after the log and the fault handlers are ready. */
void sotb_simulate_crash_from_environment(void);

#ifdef __cplusplus
}
#endif
#endif
