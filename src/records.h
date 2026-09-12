#pragma once
namespace records {
int  GetBest(int modeId);
// timeMode flips the comparison to smaller-is-better. 0 from GetBest always
// means "no record yet", never "a time of zero".
bool SetBest(int modeId, int score, bool timeMode);   // true if it was a new best
}
