#ifndef PROF_H
#define PROF_H

/* start sampling every period_us microseconds */
void prof_start(int period_us);
/* stop and print the hottest guest functions / host code lines via debugf */
void prof_report(void);

#endif
