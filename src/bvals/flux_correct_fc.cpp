//========================================================================================
// Athena++ astrophysical MHD code
// Copyright(C) 2014 James M. Stone <jmstone@princeton.edu> and other code contributors
// Licensed under the 3-clause BSD License, see LICENSE file for details
//========================================================================================
//! \file flux_correction_fc.cpp
//! \brief functions to pack/send and recv/unpack fluxes (emfs) for face-centered fields
//! (magnetic fields) at fine/coarse boundaries for the flux correction step.

#include <cstdlib>
#include <iostream>

#include "athena.hpp"
#include "globals.hpp"
#include "parameter_input.hpp"
#include "mesh/mesh.hpp"
#include "bvals.hpp"

//----------------------------------------------------------------------------------------
//! \fn void BoundaryValuesFC::PackAndSendFluxFC()
//! \brief Pack restricted fluxes of face-centered fields at fine/coarse boundaries
//! into boundary buffers and send to neighbors for flux-correction step. These fluxes
//! (e.g. EMFs) live at cell edges.
//!
//! This routine packs ALL the buffers on ALL the faces simultaneously for ALL the
//! MeshBlocks. Buffer data are then sent (via MPI) or copied directly for periodic or
//! block boundaries.

TaskStatus BoundaryValuesFC::PackAndSendFluxFC(DvceEdgeFld4D<Real> &flx) {
  // create local references for variables in kernel
  int nmb = pmy_pack->pmb->nmb;
  int nnghbr = pmy_pack->pmb->nnghbr;

  auto &cis = pmy_pack->pmesh->mb_indcs.cis;
  auto &cjs = pmy_pack->pmesh->mb_indcs.cjs;
  auto &cks = pmy_pack->pmesh->mb_indcs.cks;

  int &my_rank = global_variable::my_rank;
  auto &nghbr = pmy_pack->pmb->nghbr;
  auto &mbgid = pmy_pack->pmb->mb_gid;
  auto &mblev = pmy_pack->pmb->mb_lev;
  auto &sbuf = send_buf;
  auto &rbuf = recv_buf;
  auto &one_d = pmy_pack->pmesh->one_d;
  auto &two_d = pmy_pack->pmesh->two_d;

  // Outer loop over (# of MeshBlocks)*(# of neighbors)*(3 field components)
  Kokkos::TeamPolicy<> policy(DevExeSpace(), (3*nmb*nnghbr), Kokkos::AUTO);
  Kokkos::parallel_for("RecvBuff", policy, KOKKOS_LAMBDA(TeamMember_t tmember) {
    const int m = (tmember.league_rank())/(3*nnghbr);
    const int n = (tmember.league_rank() - m*(3*nnghbr))/3;
    const int v = (tmember.league_rank() - m*(3*nnghbr) - 3*n);

    // Note send buffer flux indices are for the coarse mesh
    const int il = sbuf[n].iflux[v].bis;
    const int iu = sbuf[n].iflux[v].bie;
    const int jl = sbuf[n].iflux[v].bjs;
    const int ju = sbuf[n].iflux[v].bje;
    const int kl = sbuf[n].iflux[v].bks;
    const int ku = sbuf[n].iflux[v].bke;
    const int ndat = sbuf[n].iflux_ndat;
    const int ni = iu - il + 1;
    const int nj = ju - jl + 1;
    const int nk = ku - kl + 1;
    const int nji  = nj*ni;
    const int nkj  = nk*nj;
    const int nki  = nk*ni;

    // indices of recv'ing (destination) MB and buffer: MB IDs are stored sequentially
    // in MeshBlockPacks, so array index equals (target_id - first_id)
    int dm = nghbr.d_view(m,n).gid - mbgid.d_view(0);
    int dn = nghbr.d_view(m,n).dest;

    // only pack buffers when neighbor is at coarser level
    if ((nghbr.d_view(m,n).gid >=0) && (nghbr.d_view(m,n).lev < mblev.d_view(m))) {
      // x1faces (only load x2e and x3e)
      if (n<8) {
        // i-index is fixed for flux correction on x1faces
        int fi = 2*il - cis;
        Kokkos::parallel_for(Kokkos::TeamThreadRange<>(tmember, nkj), [&](const int idx) {
          int k = idx / nj;
          int j = (idx - k * nj) + jl;
          k += kl;
          int fj = 2*j - cjs;
          int fk = 2*k - cks;
          if (v==1) {
            Real rflx;
            if (one_d) {
              rflx = flx.x2e(m,0,0,fi);
            } else if (two_d) {
              rflx = 0.5*(flx.x2e(m,0,fj,fi) + flx.x2e(m,0,fj+1,fi));
            } else {
              rflx = 0.5*(flx.x2e(m,fk,fj,fi) + flx.x2e(m,fk,fj+1,fi));
            }
            // copy directly into recv buffer if MeshBlocks on same rank
            if (nghbr.d_view(m,n).rank == my_rank) {
              rbuf[dn].flux(dm, ndat*v + (j-jl + nj*(k-kl))) = rflx;
            // else copy into send buffer for MPI communication below
            } else {
              sbuf[n].flux(m, ndat*v + (j-jl + nj*(k-kl))) = rflx;
            }
          } else if (v==2) {
            Real rflx;
            if (one_d) {
              rflx = flx.x3e(m,0,0,fi);
            } else if (two_d) {
              rflx = flx.x3e(m,0,fj,fi);
            } else {
              rflx = 0.5*(flx.x3e(m,fk,fj,fi) + flx.x3e(m,fk+1,fj,fi));
            }
            // copy directly into recv buffer if MeshBlocks on same rank
            if (nghbr.d_view(m,n).rank == my_rank) {
              rbuf[dn].flux(dm, ndat*v + (j-jl + nj*(k-kl))) = rflx;
            // else copy into send buffer for MPI communication below
            } else {
              sbuf[n].flux(m, ndat*v + (j-jl + nj*(k-kl))) = rflx;
            }
          }
        });
        tmember.team_barrier();

      // x2faces (only load x1e and x3e)
      } else if (n<16) {
        // j-index is fixed for flux correction on x2faces
        int fj = 2*jl - cjs;
        Kokkos::parallel_for(Kokkos::TeamThreadRange<>(tmember, nki), [&](const int idx) {
          int k = idx / ni;
          int i = (idx - k * ni) + il;
          k += kl;
          int fk = 2*k - cks;
          int fi = 2*i - cis;
          if (v==0) {
            Real rflx;
            if (two_d) {
              rflx = 0.5*(flx.x1e(m,0,fj,fi) + flx.x1e(m,0,fj,fi+1));
            } else {
              rflx = 0.5*(flx.x1e(m,fk,fj,fi) + flx.x1e(m,fk,fj,fi+1));
            }
            if (nghbr.d_view(m,n).rank == my_rank) {
              rbuf[dn].flux(dm, ndat*v + i-il + ni*(k-kl)) = rflx;
            } else {
              sbuf[n].flux(m, ndat*v + i-il + ni*(k-kl)) = rflx;
            }
          } else if (v==2) {
            Real rflx;
            if (two_d) {
              rflx = flx.x3e(m,0,fj,fi);
            } else {
              rflx = 0.5*(flx.x3e(m,fk,fj,fi) + flx.x3e(m,fk+1,fj,fi));
            }
            if (nghbr.d_view(m,n).rank == my_rank) {
              rbuf[dn].flux(dm, ndat*v + i-il + ni*(k-kl)) = rflx;
            } else {
              sbuf[n].flux(m, ndat*v + i-il + ni*(k-kl)) = rflx;
            }
          }
        });
        tmember.team_barrier();

      // x1x2 edges (only load x3e)
      } else if (n<24) {
        // i/j-index is fixed for flux correction on x1x2 edges
        int fi = 2*il - cis;
        int fj = 2*jl - cjs;
        if (v==2) {
          Kokkos::parallel_for(Kokkos::TeamThreadRange<>(tmember,nk),[&](const int idx) {
            int k = idx + kl;
            int fk = 2*k - cks;
            Real rflx;
            if (two_d) {
              rflx = flx.x3e(m,0,fj,fi);
            } else {
              rflx = 0.5*(flx.x3e(m,fk,fj,fi) + flx.x3e(m,fk+1,fj,fi));
            }
            if (nghbr.d_view(m,n).rank == my_rank) {
              rbuf[dn].flux(dm, ndat*v + (k-kl)) = rflx;
            } else {
              sbuf[n].flux(m, ndat*v + (k-kl)) = rflx;
            }
          });
        }
        tmember.team_barrier();

      // x3faces (only load x1e and x2e)
      } else if (n<32) {
        // k-index is fixed for flux correction on x3faces
        int fk = 2*kl - cks;
        Kokkos::parallel_for(Kokkos::TeamThreadRange<>(tmember, nji), [&](const int idx) {
          int j = idx / ni;
          int i = (idx - j * ni) + il;
          j += jl;
          int fi = 2*i - cis;
          int fj = 2*j - cjs;
          if (v==0) {
            Real rflx = 0.5*(flx.x1e(m,fk,fj,fi) + flx.x1e(m,fk,fj,fi+1));
            if (nghbr.d_view(m,n).rank == my_rank) {
              rbuf[dn].flux(dm, ndat*v + i-il + ni*(j-jl)) = rflx;
            } else {
              sbuf[n].flux(m, ndat*v + i-il + ni*(j-jl)) = rflx;
            }
          } else if (v==1) {
            Real rflx = 0.5*(flx.x2e(m,fk,fj,fi) + flx.x2e(m,fk,fj+1,fi));
            if (nghbr.d_view(m,n).rank == my_rank) {
              rbuf[dn].flux(dm, ndat*v + i-il + ni*(j-jl)) = rflx;
            } else {
              sbuf[n].flux(m, ndat*v + i-il + ni*(j-jl)) = rflx;
            }
          }
        });
        tmember.team_barrier();

      // x3x1 edges (only load x2e)
      } else if (n<40) {
/**
        int fi = 2*il - cis;
        int fk = 2*kl - cks;
        Kokkos::parallel_for(Kokkos::TeamThreadRange<>(tmember, nj), [&](const int idx) {
          int j = idx + jl;
          int fj = 2*j - cjs;
          if (v==1) {
            Real rflx = 0.5*(flx.x2e(m,fk,fj,fi) + flx.x2e(m,fk,fj+1,fi));
            if (nghbr.d_view(m,n).rank == my_rank) {
              rbuf[dn].flux(dm, ndat*v + (j-jl)) = rflx;
            } else {
              sbuf[n].flux(m, ndat*v + (j-jl)) = rflx;
            }
          }
        });
**/
        tmember.team_barrier();

      // x2x3 edges (only load x1e)
      } else if (n<48) {
/**
        int fj = 2*jl - cjs;
        int fk = 2*kl - cks;
        Kokkos::parallel_for(Kokkos::TeamThreadRange<>(tmember, ni), [&](const int idx) {
          int i = idx + il;
          int fi = 2*i - cis;
          if (v==0) {
            Real rflx = 0.5*(flx.x1e(m,fk,fj,fi) + flx.x1e(m,fk,fj,fi+1));
            if (nghbr.d_view(m,n).rank == my_rank) {
              rbuf[dn].flux(dm, ndat*v + i-il) = rflx;
            } else {
              sbuf[n].flux(m, ndat*v + i-il) = rflx;
            }
          }
        });
**/
        tmember.team_barrier();
      }
    }  // end if-neighbor-exists block
  });  // end par_for_outer

  // Send boundary buffer to neighboring MeshBlocks using MPI
  // Sends only occur to neighbors on faces and edges at a COARSER level

  bool no_errors=true;
  for (int m=0; m<nmb; ++m) {
    for (int n=0; n<nnghbr; ++n) {
      if ( (nghbr.h_view(m,n).gid >=0) && (nghbr.h_view(m,n).lev < mblev.h_view(m)) &&
           (n<48) ) {
        // index and rank of destination Neighbor
        int dn = nghbr.h_view(m,n).dest;
        int drank = nghbr.h_view(m,n).rank;

        // if MeshBlocks are on same rank, data already copied into receive buffer above
        // So simply set communication status tag as received.
        if (drank == my_rank) {
          int dm = nghbr.h_view(m,n).gid - pmy_pack->gids;
          rbuf[dn].flux_stat[dm] = BoundaryCommStatus::received;

#if MPI_PARALLEL_ENABLED
        // Send boundary data using MPI
        } else {
          // create tag using local ID and buffer index of *receiving* MeshBlock
          int lid = nghbr.h_view(m,n).gid - pmy_pack->pmesh->gidslist[drank];
          int tag = CreateMPITag(lid, dn);

          // get ptr to send buffer for fluxes
          int data_size = 3*(send_buf[n].iflux_ndat);
          auto send_ptr = Kokkos::subview(send_buf[n].flux, m, Kokkos::ALL);

          int ierr = MPI_Isend(send_ptr.data(), data_size, MPI_ATHENA_REAL, drank, tag,
                               flux_comm, &(send_buf[n].flux_req[m]));
          if (ierr != MPI_SUCCESS) {no_errors=false;}
#endif
        }
      }
    }
  }
  if (no_errors) return TaskStatus::complete;

  return TaskStatus::fail;
}

//----------------------------------------------------------------------------------------
//! \fn void RecvAndUnpackFluxFC()
//! \brief Unpack boundary buffers for flux correction of FC variables

TaskStatus BoundaryValuesFC::RecvAndUnpackFluxFC(DvceEdgeFld4D<Real> &flx) {
  // create local references for variables in kernel
  int nmb = pmy_pack->pmb->nmb;
  int nnghbr = pmy_pack->pmb->nnghbr;

  bool bflag = false;
  auto &nghbr = pmy_pack->pmb->nghbr;
  auto &rbuf = recv_buf;
  auto &mblev = pmy_pack->pmb->mb_lev;

#if MPI_PARALLEL_ENABLED
  // probe MPI communications.  This is a bit of black magic that seems to promote
  // communications to top of stack and gets them to complete more quickly
  int test;
  int ierr = MPI_Iprobe(MPI_ANY_SOURCE, MPI_ANY_TAG, flux_comm, &test, MPI_STATUS_IGNORE);
  if (ierr != MPI_SUCCESS) {return TaskStatus::incomplete;}
#endif

  //----- STEP 1: check that recv boundary buffer communications have all completed
  // receives only occur for neighbors on faces and edges at a FINER level

  for (int m=0; m<nmb; ++m) {
    for (int n=0; n<nnghbr; ++n) {
      if ( (nghbr.h_view(m,n).gid >=0) && (nghbr.h_view(m,n).lev > mblev.h_view(m)) &&
           (n<48) ) {
        if (nghbr.h_view(m,n).rank == global_variable::my_rank) {
          if (rbuf[n].flux_stat[m] == BoundaryCommStatus::waiting) {bflag = true;}
#if MPI_PARALLEL_ENABLED
        } else {
          MPI_Test(&(rbuf[n].flux_req[m]), &test, MPI_STATUS_IGNORE);
          if (static_cast<bool>(test)) {
            rbuf[n].flux_stat[m] = BoundaryCommStatus::received;
          } else {
            bflag = true;
          }
#endif
        }
      }
    }
  }

  // exit if recv boundary buffer communications have not completed
  if (bflag) {return TaskStatus::incomplete;}

  //----- STEP 2: buffers have all completed, so unpack

  // Outer loop over (# of MeshBlocks)*(# of neighbors)*(3 field components)
  Kokkos::TeamPolicy<> policy(DevExeSpace(), (3*nmb*nnghbr), Kokkos::AUTO);
  Kokkos::parallel_for("RecvBuff", policy, KOKKOS_LAMBDA(TeamMember_t tmember) {
    const int m = (tmember.league_rank())/(3*nnghbr);
    const int n = (tmember.league_rank() - m*(3*nnghbr))/3;
    const int v = (tmember.league_rank() - m*(3*nnghbr) - 3*n);

    // Recv buffer flux indices are for the regular mesh
    const int il = rbuf[n].iflux[v].bis;
    const int iu = rbuf[n].iflux[v].bie;
    const int jl = rbuf[n].iflux[v].bjs;
    const int ju = rbuf[n].iflux[v].bje;
    const int kl = rbuf[n].iflux[v].bks;
    const int ku = rbuf[n].iflux[v].bke;
    const int ndat = rbuf[n].iflux_ndat;
    const int ni = iu - il + 1;
    const int nj = ju - jl + 1;
    const int nk = ku - kl + 1;
    const int nji  = nj*ni;
    const int nkj  = nk*nj;
    const int nki  = nk*ni;

    // only unpack buffers for faces and edges when neighbor is at finer level
    if ((nghbr.d_view(m,n).gid >=0) && (nghbr.d_view(m,n).lev > mblev.d_view(m))) {
      // x1faces
      if (n<8) {
        Kokkos::parallel_for(Kokkos::TeamThreadRange<>(tmember, nkj), [&](const int idx) {
          int k = idx / nj;
          int j = (idx - k * nj) + jl;
          k += kl;
          if (v==1) {
            flx.x2e(m,k,j,il) = rbuf[n].flux(m,ndat*v + (j-jl + nj*(k-kl)));
          } else if (v==2) {
            flx.x3e(m,k,j,il) = rbuf[n].flux(m,ndat*v + (j-jl + nj*(k-kl)));
          }
        });
        tmember.team_barrier();

      // x2faces
      } else if (n<16) {
        Kokkos::parallel_for(Kokkos::TeamThreadRange<>(tmember, nki), [&](const int idx) {
          int k = idx/ni;
          int i = (idx - k * ni) + il;
          k += kl;
          if (v==0) {
            flx.x1e(m,k,jl,i) = rbuf[n].flux(m,ndat*v + i-il + ni*(k-kl));
          } else if (v==2) {
            flx.x3e(m,k,jl,i) = rbuf[n].flux(m,ndat*v + i-il + ni*(k-kl));
          }
        });
        tmember.team_barrier();

      // x1x2 edges
      } else if (n<24) {
        if (v==2) {
          Kokkos::parallel_for(Kokkos::TeamThreadRange<>(tmember,nk),[&](const int idx) {
            int k = idx + kl;
            flx.x3e(m,k,jl,il) = rbuf[n].flux(m,ndat*v + (k-kl));
          });
        }
        tmember.team_barrier();

      // x3faces
      } else if (n<32)  {
        Kokkos::parallel_for(Kokkos::TeamThreadRange<>(tmember, nji), [&](const int idx) {
          int j = idx / ni;
          int i = (idx - j * ni) + il;
          j += jl;
          if (v==0) {
            flx.x1e(m,kl,j,i) = rbuf[n].flux(m,ndat*v + i-il + ni*(j-jl));
          } else if (v==1) {
            flx.x2e(m,kl,j,i) = rbuf[n].flux(m,ndat*v + i-il + ni*(j-jl));
          }
        });
        tmember.team_barrier();

      // x3x1 edges
      } else if (n<40) {
/**
        Kokkos::parallel_for(Kokkos::TeamThreadRange<>(tmember, nj), [&](const int idx) {
          int j = idx + jl;
          if (v==1) {
            flx.x2e(m,kl,j,il) = rbuf[n].flux(m,ndat*v + (j-jl));
          }
        });
**/
        tmember.team_barrier();

      // x2x3 edges
      } else if (n<48) {
/**
        Kokkos::parallel_for(Kokkos::TeamThreadRange<>(tmember, ni), [&](const int idx) {
          int i = idx + il;
          if (v==0) {
            flx.x1e(m,kl,jl,i) = rbuf[n].flux(m,ndat*v + i-il);
          }
        });
**/
        tmember.team_barrier();
      }
    }  // end if-neighbor-exists block
  });  // end par_for_outer

  return TaskStatus::complete;
}

//----------------------------------------------------------------------------------------
//! \fn  void BoundaryValuesFC::InitRecvFlux
//! \brief Posts non-blocking receives (with MPI), and initialize all boundary receive
//! status flags to waiting (with or without MPI) for boundary communications of fluxes.

TaskStatus BoundaryValuesFC::InitFluxRecv(const int nvar) {
  int &nmb = pmy_pack->nmb_thispack;
  int &nnghbr = pmy_pack->pmb->nnghbr;
  auto &nghbr = pmy_pack->pmb->nghbr;
  auto &mblev = pmy_pack->pmb->mb_lev;

  // Initialize communications of fluxes
  bool no_errors=true;
  for (int m=0; m<nmb; ++m) {
    for (int n=0; n<nnghbr; ++n) {
      // only post receives for neighbors on faces and edges at FINER level
      // this is the only thing different from BoundaryValuesCC::InitRecvFlux()
      if ( (nghbr.h_view(m,n).gid >=0) && (nghbr.h_view(m,n).lev > mblev.h_view(m)) &&
           (n<48) ) {
#if MPI_PARALLEL_ENABLED
        // rank of destination buffer
        int drank = nghbr.h_view(m,n).rank;

        // post non-blocking receive if neighboring MeshBlock on a different rank
        if (drank != global_variable::my_rank) {
          // create tag using local ID and buffer index of *receiving* MeshBlock
          int tag = CreateMPITag(m, n);

          // get ptr to recv buffer when neighbor is at coarser/same/fine level
          int data_size = nvar*(recv_buf[n].iflux_ndat);
          auto recv_ptr = Kokkos::subview(recv_buf[n].flux, m, Kokkos::ALL);

          // Post non-blocking receive for this buffer on this MeshBlock
          int ierr = MPI_Irecv(recv_ptr.data(), data_size, MPI_ATHENA_REAL, drank, tag,
                               flux_comm, &(recv_buf[n].flux_req[m]));
          if (ierr != MPI_SUCCESS) {no_errors=false;}
        }
#endif
        // initialize boundary receive status flags
        recv_buf[n].flux_stat[m] = BoundaryCommStatus::waiting;
      }
    }
  }
  if (no_errors) return TaskStatus::complete;

  return TaskStatus::fail;
}

//----------------------------------------------------------------------------------------
//! \fn  void BoundaryValuesFC::ClearFluxRecv
//  \brief Waits for all MPI receives associated with boundary communcations for fluxes
//  to complete before allowing execution to continue

TaskStatus BoundaryValuesFC::ClearFluxRecv() {
  int no_errors=true;
#if MPI_PARALLEL_ENABLED
  int &nmb = pmy_pack->nmb_thispack;
  int &nnghbr = pmy_pack->pmb->nnghbr;
  auto &nghbr = pmy_pack->pmb->nghbr;

  // wait for all non-blocking receives for fluxes to finish before continuing
  for (int m=0; m<nmb; ++m) {
    for (int n=0; n<nnghbr; ++n) {
      if ( (nghbr.h_view(m,n).gid >= 0) &&
           (nghbr.h_view(m,n).rank != global_variable::my_rank) &&
           (recv_buf[n].flux_req[m] != MPI_REQUEST_NULL) ) {
        int ierr = MPI_Wait(&(recv_buf[n].flux_req[m]), MPI_STATUS_IGNORE);
        if (ierr != MPI_SUCCESS) {no_errors=false;}
      }
    }
  }
#endif
  if (no_errors) return TaskStatus::complete;

  return TaskStatus::fail;
}

//----------------------------------------------------------------------------------------
//! \fn  void BoundaryValuesFC::ClearFluxSend
//  \brief Waits for all MPI sends associated with boundary communcations for fluxes to
//   complete before allowing execution to continue

TaskStatus BoundaryValuesFC::ClearFluxSend() {
  int no_errors=true;
#if MPI_PARALLEL_ENABLED
  int &nmb = pmy_pack->nmb_thispack;
  int &nnghbr = pmy_pack->pmb->nnghbr;
  auto &nghbr = pmy_pack->pmb->nghbr;

  // wait for all non-blocking sends for fluxes to finish before continuing
  for (int m=0; m<nmb; ++m) {
    for (int n=0; n<nnghbr; ++n) {
      if ( (nghbr.h_view(m,n).gid >= 0) &&
           (nghbr.h_view(m,n).rank != global_variable::my_rank) &&
           (send_buf[n].flux_req[m] != MPI_REQUEST_NULL) ) {
        int ierr = MPI_Wait(&(send_buf[n].flux_req[m]), MPI_STATUS_IGNORE);
        if (ierr != MPI_SUCCESS) {no_errors=false;}
      }
    }
  }
#endif
  if (no_errors) return TaskStatus::complete;

  return TaskStatus::fail;
}
