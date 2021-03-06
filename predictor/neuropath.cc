/*****************************************************************
 * File: neurobranch.cc
 * Created on: 13-May-2017
 * Author: Yash Patel
 * Description: Perceptron branch predictor based on the one
 * implemented in the fast neural paths branch paper.
 ****************************************************************/

#include "cpu/pred/neuropath.hh"

#include <iostream>
#include "base/bitfield.hh"
#include "base/intmath.hh"

NeuroPathBP::NeuroPathBP(const NeuroPathBPParams *params)
  : BPredUnit(params),
	globalPredictorSize(params->globalPredictorSize),
	G (params->numThreads, 0), // 0-initialize global history, entries <=> threads
	SG(params->numThreads, 0), // 0-initialize speculative history
	globalHistoryBits(ceilLog2(params->globalPredictorSize))
{  
  if (!isPowerOf2(globalPredictorSize)) {
	fatal("Invalid global predictor size!\n");
  }

  // Set up the global history mask
  // this is equivalent to mask(log2(globalPredictorSize)
  globalHistoryMask = globalPredictorSize - 1;
	
  // Set up historyRegisterMask
  historyRegisterMask = mask(globalHistoryBits);

  // Check that predictors don't use more bits than they have available
  if (globalHistoryMask > historyRegisterMask)
	fatal("Global predictor too large for global history bits!\n");

  // speculative running total computing the perceptron output
  // each entry j corresponds to partial sum of j steps forward
  SR.assign(globalPredictorSize + 1, 0);

  // running total computing the perceptron output
  // each entry j corresponds to partial sum of j steps forward
  R.assign(globalPredictorSize + 1, 0);

  // number of hashed perceptrons, i.e. each
  // one act as a local predictor corresponding to local history
  perceptronCount = 10;

  // Perceptron theta threshold parameter empirically determined in the
  // fast neural branch predictor paper to be 2.14 * history + 20.58
  theta = 2.14 * (globalPredictorSize + 1) + 20.58;
  
  // weights per neuron (historyRegister per neuron)
  weightsTable.assign(perceptronCount,
					  std::vector<unsigned>(globalPredictorSize + 1, 0));
  
  // figure out max and min weights values
  max_weight = (1 << (globalHistoryBits - 1)) - 1;
  min_weight = -(max_weight + 1);
}

void
NeuroPathBP::btbUpdate(ThreadID tid, Addr branch_addr, void * &bp_history)
{
    //Update Global History to Not Taken (clear LSB)
    G[tid] &= (historyRegisterMask & ~ULL(1));
}

void
inline
NeuroPathBP::updatePath(Addr branch_addr)
{
  path.insert(path.begin(), branch_addr);
  // only maintains the last H (globalPredictorSize) addresses in history
  if (path.size() > (globalPredictorSize + 1)) path.pop_back();
}

unsigned
NeuroPathBP::saturatedUpdate (unsigned weight, bool inc) {
  if      ( inc && (weight < max_weight)) return weight + 1;
  else if (!inc && (weight > min_weight)) return weight - 1;
  return weight;
}

bool
NeuroPathBP::lookup(ThreadID tid, Addr branch_addr, void * &bp_history)
{
  updatePath(branch_addr);

  unsigned k_j;
  // the current perceptron weights correspond to the ones
  // being hashed from the program counter and number of perceptrons
  int curPerceptron = branch_addr % perceptronCount; 
  int y_out         = weightsTable[curPerceptron][0] +
	SR[globalPredictorSize];
  bool prediction   = (y_out >= 0);

  // Create BPHistory and pass it back to be recorded.
  BPHistory *history = new BPHistory;
  history->globalHistory   = SG[tid];
  history->globalPredTaken = prediction;
  bp_history = (void *)history;

  std::vector<unsigned> SR_prime;
  SR_prime.assign(globalPredictorSize + 1, 0);
  
  for (int j = 1; j <= globalPredictorSize; j++) {
	k_j = globalPredictorSize - j;
	SR_prime[k_j + 1] = SR[k_j];
	// case where the prediction is "taken"
	if (prediction) SR_prime[k_j + 1] += weightsTable[curPerceptron][j];
	else            SR_prime[k_j + 1] -= weightsTable[curPerceptron][j];
  }

  SR    = SR_prime;
  SR[0] = 0;
  
  SG[tid] = ((SG[tid] << 1) | prediction);
  SG[tid] = (SG[tid] & historyRegisterMask);
  return prediction;
}

void
NeuroPathBP::uncondBranch(ThreadID tid, Addr pc, void * &bp_history)
{
  // Create BPHistory and pass it back to be recorded.
  BPHistory *history = new BPHistory;
  history->globalHistory = SG[tid];
  history->globalPredTaken = true;
  history->globalUsed = true;
  bp_history = static_cast<void *>(history);

  updatePath(pc);  
  SG[tid] = ((SG[tid] << 1) | 1);
  SG[tid] &= historyRegisterMask;
}

void
NeuroPathBP::update(ThreadID tid, Addr branch_addr, bool taken,
				void *bp_history, bool squashed)
{
  assert(bp_history);
  unsigned k, k_j;
  int curPerceptron = branch_addr % perceptronCount; 
  int y_out         = weightsTable[curPerceptron][0] +
	SR[globalPredictorSize];
  
  unsigned thread_history = SG[tid];

  // maintain R in case the history got squashed
  std::vector<unsigned> R_prime;
  R_prime.assign(globalPredictorSize + 1, 0);
  
  for (int j = 1; j <= globalPredictorSize; j++) {
	k_j = globalPredictorSize - j;
	R_prime[k_j + 1] = R[k_j];
	// case where the prediction is "taken"
	if (taken) R_prime[k_j + 1] += weightsTable[curPerceptron][j];
	else       R_prime[k_j + 1] -= weightsTable[curPerceptron][j];
  }

  R    = R_prime;
  R[0] = 0;

  // Update non-speculative global history shift register
  G[tid] = ((G[tid] << 1) | taken);
  G[tid] &= historyRegisterMask;
  
  // If this is a misprediction, restore the speculatively
  // updated state (global history register and local history)
  // and update again.
  if (squashed || (abs(y_out) <= theta)) {
	if (squashed) {
	  // Global history restore and update
	  SG[tid] = G[tid];
	  SR = R;
	}
	
	weightsTable[curPerceptron][0] = saturatedUpdate(
	    weightsTable[curPerceptron][0], taken);
	for (int j = 1; j <= globalPredictorSize; j++) {
	  // weight is chosen mod path.size in the edge case of short history
	  k = (path[j % path.size()] % perceptronCount); 
	  weightsTable[k][j] = saturatedUpdate(weightsTable[k][j],
	      ((thread_history >> j) & 1) == taken);
	}
  }
}

void
NeuroPathBP::squash(ThreadID tid, void *bp_history)
{
  BPHistory *history = static_cast<BPHistory *>(bp_history);

  // Restore global history to state prior to this branch.
  SG[tid] = G[tid];

  // Restore SR to a non-speculative version computed end if
  // using only non-speculative information
  SR = R;
  
  // Delete this BPHistory now that we're done with it.
  delete history;
}

unsigned
NeuroPathBP::getGHR(ThreadID tid, void *bp_history) const
{
  return static_cast<BPHistory *>(bp_history)->globalHistory;
}

NeuroPathBP*
NeuroPathBPParams::create()
{
  return new NeuroPathBP(this);
}
