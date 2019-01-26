/*
  Stockfish, a UCI chess playing engine derived from Glaurung 2.1
  Copyright (C) 2004-2008 Tord Romstad (Glaurung author)
  Copyright (C) 2008-2015 Marco Costalba, Joona Kiiski, Tord Romstad
  Copyright (C) 2015-2019 Marco Costalba, Joona Kiiski, Gary Linscott, Tord Romstad

  Stockfish is free software: you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation, either version 3 of the License, or
  (at your option) any later version.

  Stockfish is distributed in the hope that it will be useful,
  but WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
  GNU General Public License for more details.

  You should have received a copy of the GNU General Public License
  along with this program.  If not, see <http://www.gnu.org/licenses/>.
*/

#ifdef USE_MPI

#include <array>
#include <cstddef>
#include <cstdlib>
#include <iostream>
#include <istream>
#include <string>
#include <vector>

#include "cluster.h"
#include "thread.h"
#include "tt.h"
#include "timeman.h"

namespace Cluster {

// Total number of ranks and rank within the communicator
static int world_rank = MPI_PROC_NULL;
static int world_size = 0;

// Signals between ranks exchange basic info using a dedicated communicator
static MPI_Comm signalsComm = MPI_COMM_NULL;
static MPI_Request reqSignals = MPI_REQUEST_NULL;
static uint64_t signalsCallCounter = 0;

// Signals are the number of nodes searched, stop, table base hits, transposition table saves
enum Signals : int { SIG_NODES = 0, SIG_STOP = 1, SIG_TB = 2, SIG_TTS = 3, SIG_NB = 4};
static uint64_t signalsSend[SIG_NB] = {};
static uint64_t signalsRecv[SIG_NB] = {};
static uint64_t nodesSearchedOthers = 0;
static uint64_t tbHitsOthers = 0;
static uint64_t TTsavesOthers = 0;
static uint64_t stopSignalsPosted = 0;

// The UCI threads of each rank exchange use a dedicated communicator
static MPI_Comm InputComm = MPI_COMM_NULL;

// TT entries are communicated with a dedicated communicator.
MPI_Comm TTComm = MPI_COMM_NULL;
std::atomic<uint64_t> sendRecvPosted = {};

// bestMove requires MoveInfo communicators and data types
static MPI_Comm MoveComm = MPI_COMM_NULL;
static MPI_Datatype MIDatatype = MPI_DATATYPE_NULL;

///
ClusterCache::ClusterCache() {

  for (int i : {0, 1})
  {
      TTSendRecvBuffs[i].resize(TTCacheSize * size());
      std::fill(TTSendRecvBuffs[i].begin(), TTSendRecvBuffs[i].end(), KeyedTTEntry());
  }
  reqsTTSendRecv = {MPI_REQUEST_NULL, MPI_REQUEST_NULL};
  TTCacheCounter = sendRecvPosted = 0;
}

// add an entry to the clusterCache, maintaining the heap structure
bool ClusterCache::replace(const KeyedTTEntry& value) {

  ++TTCacheCounter;
  auto compare = [](const KeyedTTEntry& lhs, const KeyedTTEntry& rhs)
                   { return lhs.second.depth() > rhs.second.depth(); };
  if (compare(value, buffer.front()))
  {
      std::pop_heap(buffer.begin(), buffer.end(), compare);
      buffer.back() = value;
      std::push_heap(buffer.begin(), buffer.end(), compare);
      return true;
  }
  return false;
}

// handle a finished communication and a full TTCache.
void ClusterCache::handle_buffer() {

   // Save all received entries to TT, and store our TTCaches, ready for the next round of communication
   for (size_t irank = 0; irank < size_t(size()) ; ++irank)
   {
       if (irank == size_t(rank())) // this is our part, fill the part of the buffer for sending
       {
          // Copy from the thread caches to the right spot in the buffer
          size_t i = irank * TTCacheSize;
          for (auto&& e : buffer)
               TTSendRecvBuffs[sendRecvPosted % 2][i++] = e;

          // Reset buffer
          buffer = {};
          TTCacheCounter = 0;
       }
       else // process data received from the corresponding rank.
          for (size_t i = irank * TTCacheSize; i < (irank + 1) * TTCacheSize; ++i)
          {
              auto&& e = TTSendRecvBuffs[sendRecvPosted % 2][i];
              bool found;
              TTEntry* replace_tte;
              replace_tte = TT.probe(e.first, found);
              replace_tte->save(e.first, e.second.value(), e.second.pv_hit(), e.second.bound(), e.second.depth(),
                                e.second.move(), e.second.eval());
          }
   }

   ++sendRecvPosted;
   MPI_Irecv(TTSendRecvBuffs[sendRecvPosted       % 2].data(),
             TTSendRecvBuffs[sendRecvPosted       % 2].size() * sizeof(KeyedTTEntry), MPI_BYTE,
             (rank() + size() - 1) % size(), 42, TTComm, &reqsTTSendRecv[0]);
   MPI_Isend(TTSendRecvBuffs[(sendRecvPosted + 1) % 2].data(),
             TTSendRecvBuffs[(sendRecvPosted + 1) % 2].size() * sizeof(KeyedTTEntry), MPI_BYTE,
             (rank() + 1         ) % size(), 42, TTComm, &reqsTTSendRecv[1]);
}

/// Initialize MPI and associated data types. Note that the MPI library must be configured
/// to support MPI_THREAD_MULTIPLE, since multiple threads access MPI simultaneously.
void init() {

  int thread_support;
  MPI_Init_thread(nullptr, nullptr, MPI_THREAD_MULTIPLE, &thread_support);
  if (thread_support < MPI_THREAD_MULTIPLE)
  {
      std::cerr << "Stockfish requires support for MPI_THREAD_MULTIPLE."
                << std::endl;
      std::exit(EXIT_FAILURE);
  }

  MPI_Comm_rank(MPI_COMM_WORLD, &world_rank);
  MPI_Comm_size(MPI_COMM_WORLD, &world_size);

  const std::array<MPI_Aint, 5> MIdisps = {offsetof(MoveInfo, move),
                                           offsetof(MoveInfo, ponder),
                                           offsetof(MoveInfo, depth),
                                           offsetof(MoveInfo, score),
                                           offsetof(MoveInfo, rank)};
  MPI_Type_create_hindexed_block(5, 1, MIdisps.data(), MPI_INT, &MIDatatype);
  MPI_Type_commit(&MIDatatype);

  MPI_Comm_dup(MPI_COMM_WORLD, &InputComm);
  MPI_Comm_dup(MPI_COMM_WORLD, &TTComm);
  MPI_Comm_dup(MPI_COMM_WORLD, &MoveComm);
  MPI_Comm_dup(MPI_COMM_WORLD, &signalsComm);
}

/// Finalize MPI and free the associated data types.
void finalize() {

  MPI_Type_free(&MIDatatype);

  MPI_Comm_free(&InputComm);
  MPI_Comm_free(&TTComm);
  MPI_Comm_free(&MoveComm);
  MPI_Comm_free(&signalsComm);

  MPI_Finalize();
}

/// Return the total number of ranks
int size() {

  return world_size;
}

/// Return the rank (index) of the process
int rank() {

  return world_rank;
}

/// As input is only received by the root (rank 0) of the cluster, this input must be relayed
/// to the UCI threads of all ranks, in order to setup the position, etc. We do this with a
/// dedicated getline implementation, where the root broadcasts to all other ranks the received
/// information.
bool getline(std::istream& input, std::string& str) {

  int size;
  std::vector<char> vec;
  bool state;

  if (is_root())
  {
      state = static_cast<bool>(std::getline(input, str));
      vec.assign(str.begin(), str.end());
      size = vec.size();
  }

  // Some MPI implementations use busy-wait polling, while we need yielding as otherwise
  // the UCI thread on the non-root ranks would be consuming resources.
  static MPI_Request reqInput = MPI_REQUEST_NULL;
  MPI_Ibcast(&size, 1, MPI_INT, 0, InputComm, &reqInput);
  if (is_root())
      MPI_Wait(&reqInput, MPI_STATUS_IGNORE);
  else
  {
      while (true)
      {
          int flag;
          MPI_Test(&reqInput, &flag, MPI_STATUS_IGNORE);
          if (flag)
              break;
          else
              std::this_thread::sleep_for(std::chrono::milliseconds(10));
      }
  }

  // Broadcast received string
  if (!is_root())
      vec.resize(size);
  MPI_Bcast(vec.data(), size, MPI_CHAR, 0, InputComm);
  if (!is_root())
      str.assign(vec.begin(), vec.end());
  MPI_Bcast(&state, 1, MPI_CXX_BOOL, 0, InputComm);

  return state;
}

/// Sending part of the signal communication loop
void signals_send() {

  signalsSend[SIG_NODES] = Threads.nodes_searched();
  signalsSend[SIG_TB] = Threads.tb_hits();
  signalsSend[SIG_TTS] = Threads.TT_saves();
  signalsSend[SIG_STOP] = Threads.stop;
  MPI_Iallreduce(signalsSend, signalsRecv, SIG_NB, MPI_UINT64_T,
                 MPI_SUM, signalsComm, &reqSignals);
  ++signalsCallCounter;
}

/// Processing part of the signal communication loop.
/// For some counters (e.g. nodes) we only keep their sum on the other nodes
/// allowing to add local counters at any time for more fine grained process,
/// which is useful to indicate progress during early iterations, and to have
/// node counts that exactly match the non-MPI code in the single rank case.
/// This call also propagates the stop signal between ranks.
void signals_process() {

  nodesSearchedOthers = signalsRecv[SIG_NODES] - signalsSend[SIG_NODES];
  tbHitsOthers = signalsRecv[SIG_TB] - signalsSend[SIG_TB];
  TTsavesOthers = signalsRecv[SIG_TTS] - signalsSend[SIG_TTS];
  stopSignalsPosted = signalsRecv[SIG_STOP];
  if (signalsRecv[SIG_STOP] > 0)
      Threads.stop = true;
}

// TODO
void sendrecv_sync(Thread * thread) {

  // TODO ... need to figure out how to properly finalize sendRecvs..

  // Finalize outstanding messages in the sendRecv loop
  // MPI_Allreduce(&sendRecvPosted, &globalCounter, 1, MPI_UINT64_T, MPI_MAX, MoveComm);
  // while (sendRecvPosted < globalCounter)
  // {
  //     MPI_Waitall(reqsTTSendRecv.size(), reqsTTSendRecv.data(), MPI_STATUSES_IGNORE);
  //     sendrecv_post();
  // }
  // assert(sendRecvPosted == globalCounter);
  // MPI_Waitall(reqsTTSendRecv.size(), reqsTTSendRecv.data(), MPI_STATUSES_IGNORE);

}

/// During search, most message passing is asynchronous, but at the end of
/// search it makes sense to bring them to a common, finalized state.
void signals_sync() {

  while(stopSignalsPosted < uint64_t(size()))
      signals_poll();

  // Finalize outstanding messages of the signal loops.
  // We might have issued one call less than needed on some ranks.
  uint64_t globalCounter;
  MPI_Allreduce(&signalsCallCounter, &globalCounter, 1, MPI_UINT64_T, MPI_MAX, MoveComm);
  if (signalsCallCounter < globalCounter)
  {
      MPI_Wait(&reqSignals, MPI_STATUS_IGNORE);
      signals_send();
  }
  assert(signalsCallCounter == globalCounter);
  MPI_Wait(&reqSignals, MPI_STATUS_IGNORE);
  signals_process();

}

/// Initialize signal counters to zero.
void signals_init() {

  stopSignalsPosted = tbHitsOthers = TTsavesOthers = nodesSearchedOthers = 0;
  signalsCallCounter = sendRecvPosted = 0;

  signalsSend[SIG_NODES] = signalsRecv[SIG_NODES] = 0;
  signalsSend[SIG_TB] = signalsRecv[SIG_TB] = 0;
  signalsSend[SIG_TTS] = signalsRecv[SIG_TTS] = 0;
  signalsSend[SIG_STOP] = signalsRecv[SIG_STOP] = 0;

}

/// Poll the signal loop, and start next round as needed.
void signals_poll() {

  int flag;
  MPI_Test(&reqSignals, &flag, MPI_STATUS_IGNORE);
  if (flag)
  {
     signals_process();
     signals_send();
  }
}

/// Provide basic info related the cluster performance, in particular, the number of signals send,
/// signals per sounds (sps), the number of gathers, the number of positions gathered (per node and per second, gpps)
/// The number of TT saves and TT saves per second. If gpps equals approximately TTSavesps the gather loop has enough bandwidth.
void cluster_info(Depth depth) {

  TimePoint elapsed = Time.elapsed() + 1;
  uint64_t TTSaves = TT_saves();

  sync_cout << "info depth " << depth / ONE_PLY << " cluster "
            << " signals " << signalsCallCounter << " sps " << signalsCallCounter * 1000 / elapsed
            << " sendRecvs " << sendRecvPosted << " srpps " <<  TTCacheSize * size() * sendRecvPosted * 1000 / elapsed
            << " TTSaves " << TTSaves << " TTSavesps " << TTSaves * 1000 / elapsed
            << sync_endl;
}

/// When a TT entry is saved, additional steps are taken if the entry is of sufficient depth.
/// If sufficient entries has been collected, a communication is initiated.
/// If a communication has been completed, the received results are saved to the TT.
void save(Thread* thread, TTEntry* tte,
          Key k, Value v, bool PvHit, Bound b, Depth d, Move m, Value ev) {

  // Standard save to the TT
  tte->save(k, v, PvHit, b, d, m, ev);

  // If the entry is of sufficient depth to be worth communicating, take action.
  if (d > 3 * ONE_PLY)
  {
     // count the TTsaves to information: this should be relatively similar
     // to the number of entries we can send/recv.
     thread->TTsaves.fetch_add(1, std::memory_order_relaxed);

     // Add to thread's send buffer
     thread->ttCache.replace(KeyedTTEntry(k,*tte));

     // Try to communicate as soon we have collected sufficient data
     if (thread->ttCache.TTCacheCounter >= TTCacheSize)
     {
         // Test communication status
         int flag;
         MPI_Testall(thread->ttCache.reqsTTSendRecv.size(), thread->ttCache.reqsTTSendRecv.data(), &flag, MPI_STATUSES_IGNORE);

         // Current communication is complete
         if (flag)
         {
             thread->ttCache.handle_buffer();
             ++sendRecvPosted;  // TODO needed for final sync only. Duplicates the threadLocal counters.

	     // Force check of time on the next occasion, the above actions might have taken some time.
             if (thread == Threads.main())
                 static_cast<MainThread*>(thread)->callsCnt = 0;

         }
     }
  }
}

/// Picks the bestMove across ranks, and send the associated info and PV to the root of the cluster.
/// Note that this bestMove and PV must be output by the root, the guarantee proper ordering of output.
/// TODO update to the scheme in master.. can this use aggregation of votes?
void pick_moves(MoveInfo& mi, std::string& PVLine) {

  MoveInfo* pMoveInfo = NULL;
  if (is_root())
  {
      pMoveInfo = (MoveInfo*)malloc(sizeof(MoveInfo) * size());
  }
  MPI_Gather(&mi, 1, MIDatatype, pMoveInfo, 1, MIDatatype, 0, MoveComm);

  if (is_root())
  {
      std::map<int, int> votes;
      int minScore = pMoveInfo[0].score;
      for (int i = 0; i < size(); ++i)
      {
          minScore = std::min(minScore, pMoveInfo[i].score);
          votes[pMoveInfo[i].move] = 0;
      }
      for (int i = 0; i < size(); ++i)
      {
          votes[pMoveInfo[i].move] += pMoveInfo[i].score - minScore + pMoveInfo[i].depth;
      }
      int bestVote = votes[pMoveInfo[0].move];
      for (int i = 0; i < size(); ++i)
      {
          if (votes[pMoveInfo[i].move] > bestVote)
          {
              bestVote = votes[pMoveInfo[i].move];
              mi = pMoveInfo[i];
          }
      }
      free(pMoveInfo);
  }

  // Send around the final result
  MPI_Bcast(&mi, 1, MIDatatype, 0, MoveComm);

  // Send PV line to root as needed
  if (mi.rank != 0 && mi.rank == rank()) {
      int size;
      std::vector<char> vec;
      vec.assign(PVLine.begin(), PVLine.end());
      size = vec.size();
      MPI_Send(&size, 1, MPI_INT, 0, 42, MoveComm);
      MPI_Send(vec.data(), size, MPI_CHAR, 0, 42, MoveComm);
  }
  if (mi.rank != 0 && is_root()) {
      int size;
      std::vector<char> vec;
      MPI_Recv(&size, 1, MPI_INT, mi.rank, 42, MoveComm, MPI_STATUS_IGNORE);
      vec.resize(size);
      MPI_Recv(vec.data(), size, MPI_CHAR, mi.rank, 42, MoveComm, MPI_STATUS_IGNORE);
      PVLine.assign(vec.begin(), vec.end());
  }

}

/// Return nodes searched (lazily updated cluster wide in the signal loop)
uint64_t nodes_searched() {

  return nodesSearchedOthers + Threads.nodes_searched();
}

/// Return table base hits (lazily updated cluster wide in the signal loop)
uint64_t tb_hits() {

  return tbHitsOthers + Threads.tb_hits();
}

/// Return the number of saves to the TT buffers, (lazily updated cluster wide in the signal loop)
uint64_t TT_saves() {

  return TTsavesOthers + Threads.TT_saves();
}


}

#else

#include "cluster.h"
#include "thread.h"

namespace Cluster {

uint64_t nodes_searched() {

  return Threads.nodes_searched();
}

uint64_t tb_hits() {

  return Threads.tb_hits();
}

uint64_t TT_saves() {

  return Threads.TT_saves();
}

}

#endif // USE_MPI
