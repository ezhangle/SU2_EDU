/*!
 * \file integration_time.cpp
 * \brief Time deppending numerical method.
 * \author Aerospace Design Laboratory (Stanford University).
 * \version 1.2.0
 *
 * SU2 EDU, Copyright (C) 2014 Aerospace Design Laboratory (Stanford University).
 *
 * SU2 EDU is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * SU2 EDU is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with SU2 EDU. If not, see <http://www.gnu.org/licenses/>.
 */

#include "../include/integration_structure.hpp"

CMultiGridIntegration::CMultiGridIntegration(CConfig *config) : CIntegration(config) {}

CMultiGridIntegration::~CMultiGridIntegration(void) { }

void CMultiGridIntegration::MultiGrid_Iteration(CGeometry **geometry,
                                                CSolver ***solver_container,
                                                CNumerics ****numerics_container,
                                                CConfig *config,
                                                unsigned short RunTime_EqSystem,
                                                unsigned long Iteration) {
  unsigned short FinestMesh, iMGLevel;
  double monitor = 1.0;
  
  const bool restart = (config->GetRestart() || config->GetRestart_Flow());
  const bool startup_multigrid = ((config->GetRestart_Flow())     &&
                                  (RunTime_EqSystem == RUNTIME_FLOW_SYS) &&
                                  (Iteration == 0));
  const bool direct = ((config->GetKind_Solver() == EULER)                         ||
                       (config->GetKind_Solver() == NAVIER_STOKES)                 ||
                       (config->GetKind_Solver() == RANS));
  const unsigned short SolContainer_Position = config->GetContainerPosition(RunTime_EqSystem);
  
  /*--- If low fidelity simulation ---*/
  
  if (config->GetLowFidelitySim())
    config->SetFinestMesh(MESH_1);
  
  /*--- If restart, update multigrid levels at the first multigrid iteration ---*/
  
  if ((restart && (Iteration == config->GetnStartUpIter())) || startup_multigrid) {
    for (iMGLevel = 0; iMGLevel < config->GetMGLevels(); iMGLevel++) {
      SetRestricted_Solution(RunTime_EqSystem, solver_container[iMGLevel][SolContainer_Position],
                             solver_container[iMGLevel+1][SolContainer_Position],
                             geometry[iMGLevel], geometry[iMGLevel+1], config);
    }
  }
  
  /*--- Full multigrid strategy and start up with fine grid only works with the direct problem ---*/
  
  if (!config->GetRestart() && config->GetFullMG() &&  direct && ( GetConvergence_FullMG() && (config->GetFinestMesh() != MESH_0 ))) {
    SetProlongated_Solution(RunTime_EqSystem, solver_container[config->GetFinestMesh()-1][SolContainer_Position],
                            solver_container[config->GetFinestMesh()][SolContainer_Position],
                            geometry[config->GetFinestMesh()-1], geometry[config->GetFinestMesh()],
                            config);
    config->SubtractFinestMesh();
  }
  
  /*--- Set the current finest grid (full multigrid strategy) ---*/
  
  FinestMesh = config->GetFinestMesh();
  
  /*--- Perform the Full Approximation Scheme multigrid ---*/
  
  if (config->GetKind_Linear_Solver() != MULTIGRID)
    MultiGrid_Cycle(geometry, solver_container, numerics_container, config,
                    FinestMesh, config->GetMGCycle(), RunTime_EqSystem,
                    Iteration);
  else
    SingleGrid_Iteration(geometry, solver_container, numerics_container, config,
                         RunTime_EqSystem, Iteration);
  
  /*--- Computes primitive variables and gradients in the finest mesh (useful for the next solver (turbulence) and output ---*/
  
  solver_container[MESH_0][SolContainer_Position]->Preprocessing(geometry[MESH_0],
                                                                        solver_container[MESH_0], config,
                                                                        MESH_0, NO_RK_ITER, RunTime_EqSystem, true);
 
  /*--- Compute non-dimensional parameters and the convergence monitor ---*/
  
  NonDimensional_Parameters(geometry, solver_container,
                            numerics_container, config,
                            FinestMesh, RunTime_EqSystem, Iteration, &monitor);
  
  /*--- Convergence strategy ---*/
  
  Convergence_Monitoring(geometry[FinestMesh], config, Iteration, monitor);
  
}

void CMultiGridIntegration::SingleGrid_Iteration(CGeometry **geometry, CSolver ***solver_container,
                                                  CNumerics ****numerics_container, CConfig *config, unsigned short RunTime_EqSystem, unsigned long Iteration) {
  unsigned short iMesh;
  
  unsigned short SolContainer_Position = config->GetContainerPosition(RunTime_EqSystem);
  
  for (iMesh = 0; iMesh <= config->GetMGLevels(); iMesh++) {
    
    /*--- Preprocessing ---*/
    
    solver_container[iMesh][SolContainer_Position]->Preprocessing(geometry[iMesh], solver_container[iMesh], config, iMesh, 0, RunTime_EqSystem, false);
    
    /*--- Set the old solution ---*/
    
    solver_container[iMesh][SolContainer_Position]->Set_OldSolution(geometry[iMesh]);
    
    /*--- Time step evaluation ---*/
    
    solver_container[iMesh][SolContainer_Position]->SetTime_Step(geometry[iMesh], solver_container[iMesh], config, iMesh, 0);
    
    /*--- Space integration ---*/
    
    Space_Integration(geometry[iMesh], solver_container[iMesh], numerics_container[iMesh][SolContainer_Position],
                      config, iMesh, NO_RK_ITER, RunTime_EqSystem);
    
    
    /*--- Modify matrix diagonal to assure diagonal dominance on the coarse levels ---*/
    
    if (iMesh != MESH_0) {
      for (unsigned long iPoint = 0; iPoint < geometry[iMesh]->GetnPointDomain(); iPoint++) {
        double Vol = geometry[iMesh]->node[iPoint]->GetVolume();
        double Delta = Vol / solver_container[iMesh][SolContainer_Position]->node[iPoint]->GetDelta_Time();
        solver_container[iMesh][SolContainer_Position]->Jacobian.AddVal2Diag(iPoint, Delta);
      }
    }
    
    /*--- Copy the solution to the coarse level ---*/

    if (iMesh != config->GetMGLevels()) {
      SetRestricted_Solution(RunTime_EqSystem, solver_container[iMesh][SolContainer_Position], solver_container[iMesh+1][SolContainer_Position], geometry[iMesh], geometry[iMesh+1], config);
    }
    
  }
  
  /*--- Time integration ---*/
  
  Time_Integration(geometry, solver_container, config, NO_RK_ITER,
                   RunTime_EqSystem, Iteration, MESH_0);
  
  /*--- Postprocessing ---*/
  
  solver_container[MESH_0][SolContainer_Position]->Postprocessing(geometry[MESH_0], solver_container[MESH_0], config, MESH_0);
  
}

void CMultiGridIntegration::MultiGrid_Cycle(CGeometry **geometry,
                                            CSolver ***solver_container,
                                            CNumerics ****numerics_container,
                                            CConfig *config,
                                            unsigned short iMesh,
                                            unsigned short mu,
                                            unsigned short RunTime_EqSystem,
                                            unsigned long Iteration) {
  
  unsigned short iPreSmooth, iPostSmooth, iRKStep, iRKLimit = 1;
  bool startup_multigrid = (config->GetRestart_Flow() && (RunTime_EqSystem == RUNTIME_FLOW_SYS) && (Iteration == 0));
  unsigned short SolContainer_Position = config->GetContainerPosition(RunTime_EqSystem);
  
  /*--- Do a presmoothing on the grid iMesh to be restricted to the grid iMesh+1 ---*/
  
  for (iPreSmooth = 0; iPreSmooth < config->GetMG_PreSmooth(iMesh); iPreSmooth++) {
    
    switch (config->GetKind_TimeIntScheme()) {
      case RUNGE_KUTTA_EXPLICIT: iRKLimit = config->GetnRKStep(); break;
      case EULER_EXPLICIT: case EULER_IMPLICIT: iRKLimit = 1; break; }
    
    /*--- Time and space integration ---*/
    
    for (iRKStep = 0; iRKStep < iRKLimit; iRKStep++) {
      
      /*--- Send-Receive boundary conditions, and preprocessing ---*/
      
      solver_container[iMesh][SolContainer_Position]->Preprocessing(geometry[iMesh], solver_container[iMesh], config, iMesh, iRKStep, RunTime_EqSystem, false);
      
      if (iRKStep == 0) {
        
        /*--- Set the old solution ---*/
        
        solver_container[iMesh][SolContainer_Position]->Set_OldSolution(geometry[iMesh]);
        
        /*--- Compute time step, max eigenvalue, and integration scheme (steady and unsteady problems) ---*/
        
        solver_container[iMesh][SolContainer_Position]->SetTime_Step(geometry[iMesh], solver_container[iMesh], config, iMesh, Iteration);
        
      }
      
      /*--- Space integration ---*/
      
      Space_Integration(geometry[iMesh], solver_container[iMesh], numerics_container[iMesh][SolContainer_Position], config, iMesh, iRKStep, RunTime_EqSystem);
      
      /*--- Time integration, update solution using the old solution plus the solution increment ---*/
      
      Time_Integration(geometry, solver_container, config, iRKStep, RunTime_EqSystem, Iteration, iMesh);
      
      /*--- Send-Receive boundary conditions, and postprocessing ---*/
      
      solver_container[iMesh][SolContainer_Position]->Postprocessing(geometry[iMesh], solver_container[iMesh], config, iMesh);
      
    }
    
  }
  
  /*--- Compute Forcing Term $P_(k+1) = I^(k+1)_k(P_k+F_k(u_k))-F_(k+1)(I^(k+1)_k u_k)$ and update solution for multigrid ---*/
  
  if ( (iMesh < config->GetMGLevels() && ((Iteration >= config->GetnStartUpIter()) || startup_multigrid)) ) {
    
    /*--- Compute $r_k = P_k + F_k(u_k)$ ---*/
    
    solver_container[iMesh][SolContainer_Position]->Preprocessing(geometry[iMesh], solver_container[iMesh], config, iMesh, NO_RK_ITER, RunTime_EqSystem, false);
    
    Space_Integration(geometry[iMesh], solver_container[iMesh], numerics_container[iMesh][SolContainer_Position], config, iMesh, NO_RK_ITER, RunTime_EqSystem);
    
    SetResidual_Term(geometry[iMesh], solver_container[iMesh][SolContainer_Position]);
    
    /*--- Compute $r_(k+1) = F_(k+1)(I^(k+1)_k u_k)$ ---*/
    
    SetRestricted_Solution(RunTime_EqSystem, solver_container[iMesh][SolContainer_Position], solver_container[iMesh+1][SolContainer_Position], geometry[iMesh], geometry[iMesh+1], config);
    
    solver_container[iMesh+1][SolContainer_Position]->Preprocessing(geometry[iMesh+1], solver_container[iMesh+1], config, iMesh+1, NO_RK_ITER, RunTime_EqSystem, false);
    
    Space_Integration(geometry[iMesh+1], solver_container[iMesh+1], numerics_container[iMesh+1][SolContainer_Position], config, iMesh+1, NO_RK_ITER, RunTime_EqSystem);
    
    /*--- Compute $P_(k+1) = I^(k+1)_k(r_k) - r_(k+1) ---*/
    
    SetForcing_Term(solver_container[iMesh][SolContainer_Position], solver_container[iMesh+1][SolContainer_Position], geometry[iMesh], geometry[iMesh+1], config);
    
    /*--- Recursive call to MultiGrid_Cycle ---*/
    
    for (unsigned short imu = 0; imu <= mu; imu++) {
      if (iMesh == config->GetMGLevels()-2) MultiGrid_Cycle(geometry, solver_container, numerics_container, config, iMesh+1, 0, RunTime_EqSystem, Iteration);
      else MultiGrid_Cycle(geometry, solver_container, numerics_container, config, iMesh+1, mu, RunTime_EqSystem, Iteration);
    }
    
    /*--- Compute prolongated solution, and smooth the correction $u^(new)_k = u_k +  Smooth(I^k_(k+1)(u_(k+1)-I^(k+1)_k u_k))$ ---*/
    
    GetProlongated_Correction(RunTime_EqSystem, solver_container[iMesh][SolContainer_Position], solver_container[iMesh+1][SolContainer_Position],
                              geometry[iMesh],geometry[iMesh+1], config);
    
    SmoothProlongated_Correction(RunTime_EqSystem, solver_container[iMesh][SolContainer_Position], geometry[iMesh],
                                 config->GetMG_CorrecSmooth(iMesh), 1.25, config);
    
    SetProlongated_Correction(solver_container[iMesh][SolContainer_Position], geometry[iMesh], config);
    
    /*--- Solution postsmoothing in the prolongated grid ---*/
    
    for (iPostSmooth = 0; iPostSmooth < config->GetMG_PostSmooth(iMesh); iPostSmooth++) {
      
      switch (config->GetKind_TimeIntScheme()) {
        case RUNGE_KUTTA_EXPLICIT: iRKLimit = config->GetnRKStep(); break;
        case EULER_EXPLICIT: case EULER_IMPLICIT: iRKLimit = 1; break; }
      
      for (iRKStep = 0; iRKStep < iRKLimit; iRKStep++) {
        
        solver_container[iMesh][SolContainer_Position]->Preprocessing(geometry[iMesh], solver_container[iMesh], config, iMesh, iRKStep, RunTime_EqSystem, false);
        
        if (iRKStep == 0) {
          
          solver_container[iMesh][SolContainer_Position]->Set_OldSolution(geometry[iMesh]);
          
          solver_container[iMesh][SolContainer_Position]->SetTime_Step(geometry[iMesh], solver_container[iMesh], config, iMesh, Iteration);
        }
        
        Space_Integration(geometry[iMesh], solver_container[iMesh], numerics_container[iMesh][SolContainer_Position], config, iMesh, iRKStep, RunTime_EqSystem);
        
        Time_Integration(geometry, solver_container, config, iRKStep, RunTime_EqSystem, Iteration, iMesh);
        
        solver_container[iMesh][SolContainer_Position]->Postprocessing(geometry[iMesh], solver_container[iMesh], config, iMesh);
        
      }
    }
  }
  
}

void CMultiGridIntegration::GetProlongated_Correction(unsigned short RunTime_EqSystem, CSolver *sol_fine, CSolver *sol_coarse, CGeometry *geo_fine,
                                                      CGeometry *geo_coarse, CConfig *config) {
  unsigned long Point_Fine, Point_Coarse, iVertex;
  unsigned short Boundary, iMarker, iChildren, iVar;
  double Area_Parent, Area_Children, *Solution_Fine, *Solution_Coarse;
  
  const unsigned short nVar = sol_coarse->GetnVar();
  
  double *Solution = new double[nVar];
  
  for (Point_Coarse = 0; Point_Coarse < geo_coarse->GetnPointDomain(); Point_Coarse++) {
    
    Area_Parent = geo_coarse->node[Point_Coarse]->GetVolume();
    
    for (iVar = 0; iVar < nVar; iVar++) Solution[iVar] = 0.0;
    
    for (iChildren = 0; iChildren < geo_coarse->node[Point_Coarse]->GetnChildren_CV(); iChildren++) {
      Point_Fine = geo_coarse->node[Point_Coarse]->GetChildren_CV(iChildren);
      Area_Children = geo_fine->node[Point_Fine]->GetVolume();
      Solution_Fine = sol_fine->node[Point_Fine]->GetSolution();
      for (iVar = 0; iVar < nVar; iVar++)
        Solution[iVar] -= Solution_Fine[iVar]*Area_Children/Area_Parent;
    }
    
    Solution_Coarse = sol_coarse->node[Point_Coarse]->GetSolution();
    
    for (iVar = 0; iVar < nVar; iVar++)
      Solution[iVar] += Solution_Coarse[iVar];
    
    for (iVar = 0; iVar < nVar; iVar++)
      sol_coarse->node[Point_Coarse]->SetSolution_Old(Solution);
    
  }
  
  /*--- Remove any contributions from no-slip walls. ---*/
  for (iMarker = 0; iMarker < config->GetnMarker_All(); iMarker++) {
    Boundary = config->GetMarker_All_KindBC(iMarker);
    if ((Boundary == HEAT_FLUX             ) ||
        (Boundary == HEAT_FLUX_CATALYTIC   ) ||
        (Boundary == HEAT_FLUX_NONCATALYTIC) ||
        (Boundary == ISOTHERMAL            ) ||
        (Boundary == ISOTHERMAL_CATALYTIC  ) ||
        (Boundary == ISOTHERMAL_NONCATALYTIC)) {
      for(iVertex = 0; iVertex < geo_coarse->nVertex[iMarker]; iVertex++) {
        
        Point_Coarse = geo_coarse->vertex[iMarker][iVertex]->GetNode();
        
        /*--- For dirichlet boundary condtions, set the correction to zero.
         Note that Solution_Old stores the correction not the actual value ---*/
        sol_coarse->node[Point_Coarse]->SetVelSolutionOldZero();
        
      }
    }
  }
  
  for (Point_Coarse = 0; Point_Coarse < geo_coarse->GetnPointDomain(); Point_Coarse++) {
    for (iChildren = 0; iChildren < geo_coarse->node[Point_Coarse]->GetnChildren_CV(); iChildren++) {
      Point_Fine = geo_coarse->node[Point_Coarse]->GetChildren_CV(iChildren);
      sol_fine->LinSysRes.SetBlock(Point_Fine, sol_coarse->node[Point_Coarse]->GetSolution_Old());
    }
  }
  
  delete [] Solution;
  
}

void CMultiGridIntegration::SmoothProlongated_Correction (unsigned short RunTime_EqSystem, CSolver *solver, CGeometry *geometry,
                                                          unsigned short val_nSmooth, double val_smooth_coeff, CConfig *config) {
  double *Residual_Old, *Residual_Sum, *Residual, *Residual_i, *Residual_j;
  unsigned short iVar, iSmooth, iMarker, nneigh;
  unsigned long iEdge, iPoint, jPoint, iVertex;
  
  const unsigned short nVar = solver->GetnVar();
  
  if (val_nSmooth > 0) {
    
    Residual = new double [nVar];
    
    for (iPoint = 0; iPoint < geometry->GetnPoint(); iPoint++) {
      Residual_Old = solver->LinSysRes.GetBlock(iPoint);
      solver->node[iPoint]->SetResidual_Old(Residual_Old);
    }
    
    /*--- Jacobi iterations ---*/
    
    for (iSmooth = 0; iSmooth < val_nSmooth; iSmooth++) {
      for (iPoint = 0; iPoint < geometry->GetnPoint(); iPoint++)
        solver->node[iPoint]->SetResidualSumZero();
      
      /*--- Loop over Interior edges ---*/
      
      for(iEdge = 0; iEdge < geometry->GetnEdge(); iEdge++) {
        iPoint = geometry->edge[iEdge]->GetNode(0);
        jPoint = geometry->edge[iEdge]->GetNode(1);
        
        Residual_i = solver->LinSysRes.GetBlock(iPoint);
        Residual_j = solver->LinSysRes.GetBlock(jPoint);
        
        /*--- Accumulate nearest neighbor Residual to Res_sum for each variable ---*/
        
        solver->node[iPoint]->AddResidual_Sum(Residual_j);
        solver->node[jPoint]->AddResidual_Sum(Residual_i);
      }
      
      /*--- Loop over all mesh points (Update Residuals with averaged sum) ---*/
      
      for (iPoint = 0; iPoint < geometry->GetnPoint(); iPoint++) {
        nneigh = geometry->node[iPoint]->GetnPoint();
        Residual_Sum = solver->node[iPoint]->GetResidual_Sum();
        Residual_Old = solver->node[iPoint]->GetResidual_Old();
        for (iVar = 0; iVar < nVar; iVar++) {
          Residual[iVar] =(Residual_Old[iVar] + val_smooth_coeff*Residual_Sum[iVar])
          /(1.0 + val_smooth_coeff*double(nneigh));
        }
        solver->LinSysRes.SetBlock(iPoint, Residual);
      }
      
      /*--- Copy boundary values ---*/
      
      for(iMarker = 0; iMarker < geometry->GetnMarker(); iMarker++) {
        for(iVertex = 0; iVertex < geometry->GetnVertex(iMarker); iVertex++) {
          iPoint = geometry->vertex[iMarker][iVertex]->GetNode();
          Residual_Old = solver->node[iPoint]->GetResidual_Old();
          solver->LinSysRes.SetBlock(iPoint, Residual_Old);
        }
      }
      
    }
    
    delete [] Residual;
    
  }
  
}

void CMultiGridIntegration::Smooth_Solution(unsigned short RunTime_EqSystem, CSolver *solver, CGeometry *geometry,
                                            unsigned short val_nSmooth, double val_smooth_coeff, CConfig *config) {
  double *Solution_Old, *Solution_Sum, *Solution, *Solution_i, *Solution_j;
  unsigned short iVar, iSmooth, iMarker, nneigh;
  unsigned long iEdge, iPoint, jPoint, iVertex;
  
  const unsigned short nVar = solver->GetnVar();
  
  if (val_nSmooth > 0) {
    
    Solution = new double [nVar];
    
    for (iPoint = 0; iPoint < geometry->GetnPoint(); iPoint++) {
      Solution_Old = solver->node[iPoint]->GetSolution();
      solver->node[iPoint]->SetResidual_Old(Solution_Old);
    }
    
    /*--- Jacobi iterations ---*/
    for (iSmooth = 0; iSmooth < val_nSmooth; iSmooth++) {
      for (iPoint = 0; iPoint < geometry->GetnPoint(); iPoint++)
        solver->node[iPoint]->SetResidualSumZero();
      
      /*--- Loop over Interior edges ---*/
      for(iEdge = 0; iEdge < geometry->GetnEdge(); iEdge++) {
        iPoint = geometry->edge[iEdge]->GetNode(0);
        jPoint = geometry->edge[iEdge]->GetNode(1);
        
        Solution_i = solver->node[iPoint]->GetSolution();
        Solution_j = solver->node[jPoint]->GetSolution();
        
        /*--- Accumulate nearest neighbor Residual to Res_sum for each variable ---*/
        solver->node[iPoint]->AddResidual_Sum(Solution_j);
        solver->node[jPoint]->AddResidual_Sum(Solution_i);
      }
      
      /*--- Loop over all mesh points (Update Residuals with averaged sum) ---*/
      for (iPoint = 0; iPoint < geometry->GetnPoint(); iPoint++) {
        nneigh = geometry->node[iPoint]->GetnPoint();
        Solution_Sum = solver->node[iPoint]->GetResidual_Sum();
        Solution_Old = solver->node[iPoint]->GetResidual_Old();
        for (iVar = 0; iVar < nVar; iVar++) {
          Solution[iVar] =(Solution_Old[iVar] + val_smooth_coeff*Solution_Sum[iVar])
          /(1.0 + val_smooth_coeff*double(nneigh));
        }
        solver->node[iPoint]->SetSolution(Solution);
      }
      
      /*--- Copy boundary values ---*/
      for(iMarker = 0; iMarker < geometry->GetnMarker(); iMarker++)
        for(iVertex = 0; iVertex < geometry->GetnVertex(iMarker); iVertex++) {
          iPoint = geometry->vertex[iMarker][iVertex]->GetNode();
          Solution_Old = solver->node[iPoint]->GetResidual_Old();
          solver->node[iPoint]->SetSolution(Solution_Old);
        }
    }
    
    delete [] Solution;
    
  }
  
}

void CMultiGridIntegration::SetProlongated_Correction(CSolver *sol_fine, CGeometry *geo_fine, CConfig *config) {
  unsigned long Point_Fine;
  unsigned short iVar;
  double *Solution_Fine, *Residual_Fine;
  
  const unsigned short nVar = sol_fine->GetnVar();
  
  double *Solution = new double [nVar];
  
  for (Point_Fine = 0; Point_Fine < geo_fine->GetnPointDomain(); Point_Fine++) {
    Residual_Fine = sol_fine->LinSysRes.GetBlock(Point_Fine);
    Solution_Fine = sol_fine->node[Point_Fine]->GetSolution();
    for (iVar = 0; iVar < nVar; iVar++) {
      /*--- Prevent a fine grid divergence due to a coarse grid divergence ---*/
      if (Residual_Fine[iVar] != Residual_Fine[iVar]) Residual_Fine[iVar] = 0.0;
      Solution[iVar] = Solution_Fine[iVar]+config->GetDamp_Correc_Prolong()*Residual_Fine[iVar];
    }
    sol_fine->node[Point_Fine]->SetSolution(Solution);
  }
  
  delete [] Solution;
}


void CMultiGridIntegration::SetProlongated_Solution(unsigned short RunTime_EqSystem, CSolver *sol_fine, CSolver *sol_coarse, CGeometry *geo_fine, CGeometry *geo_coarse, CConfig *config) {
  unsigned long Point_Fine, Point_Coarse;
  unsigned short iChildren;
  
  for (Point_Coarse = 0; Point_Coarse < geo_coarse->GetnPointDomain(); Point_Coarse++) {
    for (iChildren = 0; iChildren < geo_coarse->node[Point_Coarse]->GetnChildren_CV(); iChildren++) {
      Point_Fine = geo_coarse->node[Point_Coarse]->GetChildren_CV(iChildren);
      sol_fine->node[Point_Fine]->SetSolution(sol_coarse->node[Point_Coarse]->GetSolution());
    }
  }
}

void CMultiGridIntegration::SetForcing_Term(CSolver *sol_fine, CSolver *sol_coarse, CGeometry *geo_fine, CGeometry *geo_coarse, CConfig *config) {
  unsigned long Point_Fine, Point_Coarse, iVertex;
  unsigned short iMarker, iVar, iChildren;
  double *Residual_Fine;
  
  const unsigned short nVar = sol_coarse->GetnVar();
  
  double *Residual = new double[nVar];
  
  for (Point_Coarse = 0; Point_Coarse < geo_coarse->GetnPointDomain(); Point_Coarse++) {
    sol_coarse->node[Point_Coarse]->SetRes_TruncErrorZero();
    
    for (iVar = 0; iVar < nVar; iVar++) Residual[iVar] = 0.0;
    for (iChildren = 0; iChildren < geo_coarse->node[Point_Coarse]->GetnChildren_CV(); iChildren++) {
      Point_Fine = geo_coarse->node[Point_Coarse]->GetChildren_CV(iChildren);
      Residual_Fine = sol_fine->LinSysRes.GetBlock(Point_Fine);
      for (iVar = 0; iVar < nVar; iVar++)
        Residual[iVar] += config->GetDamp_Res_Restric()*Residual_Fine[iVar];
    }
    sol_coarse->node[Point_Coarse]->AddRes_TruncError(Residual);
  }
  
  for (iMarker = 0; iMarker < config->GetnMarker_All(); iMarker++) {
    if ((config->GetMarker_All_KindBC(iMarker) == HEAT_FLUX              ) ||
        (config->GetMarker_All_KindBC(iMarker) == HEAT_FLUX_CATALYTIC    ) ||
        (config->GetMarker_All_KindBC(iMarker) == HEAT_FLUX_NONCATALYTIC ) ||
        (config->GetMarker_All_KindBC(iMarker) == ISOTHERMAL             ) ||
        (config->GetMarker_All_KindBC(iMarker) == ISOTHERMAL_CATALYTIC   ) ||
        (config->GetMarker_All_KindBC(iMarker) == ISOTHERMAL_NONCATALYTIC)) {
      for(iVertex = 0; iVertex < geo_coarse->nVertex[iMarker]; iVertex++) {
        Point_Coarse = geo_coarse->vertex[iMarker][iVertex]->GetNode();
        sol_coarse->node[Point_Coarse]->SetVel_ResTruncError_Zero();
      }
    }
  }
  
  for(Point_Coarse = 0; Point_Coarse < geo_coarse->GetnPointDomain(); Point_Coarse++) {
    sol_coarse->node[Point_Coarse]->SubtractRes_TruncError(sol_coarse->LinSysRes.GetBlock(Point_Coarse));
  }
  
  delete [] Residual;
}

void CMultiGridIntegration::SetResidual_Term(CGeometry *geometry, CSolver *solver) {
  unsigned long iPoint;
  
  for (iPoint = 0; iPoint < geometry->GetnPointDomain(); iPoint++)
    solver->LinSysRes.AddBlock(iPoint, solver->node[iPoint]->GetResTruncError());
  
}

void CMultiGridIntegration::SetRestricted_Residual(CSolver *sol_fine, CSolver *sol_coarse, CGeometry *geo_fine, CGeometry *geo_coarse, CConfig *config) {
  unsigned long iVertex, Point_Fine, Point_Coarse;
  unsigned short iMarker, iVar, iChildren;
  double *Residual_Fine;
  
  const unsigned short nVar = sol_coarse->GetnVar();
  
  double *Residual = new double[nVar];
  
  for (Point_Coarse = 0; Point_Coarse < geo_coarse->GetnPointDomain(); Point_Coarse++) {
    sol_coarse->node[Point_Coarse]->SetRes_TruncErrorZero();
    
    for (iVar = 0; iVar < nVar; iVar++) Residual[iVar] = 0.0;
    for (iChildren = 0; iChildren < geo_coarse->node[Point_Coarse]->GetnChildren_CV(); iChildren++) {
      Point_Fine = geo_coarse->node[Point_Coarse]->GetChildren_CV(iChildren);
      Residual_Fine = sol_fine->LinSysRes.GetBlock(Point_Fine);
      for (iVar = 0; iVar < nVar; iVar++)
        Residual[iVar] += Residual_Fine[iVar];
    }
    sol_coarse->node[Point_Coarse]->AddRes_TruncError(Residual);
  }
  
  for (iMarker = 0; iMarker < config->GetnMarker_All(); iMarker++) {
    if ((config->GetMarker_All_KindBC(iMarker) == HEAT_FLUX              ) ||
        (config->GetMarker_All_KindBC(iMarker) == HEAT_FLUX_CATALYTIC    ) ||
        (config->GetMarker_All_KindBC(iMarker) == HEAT_FLUX_NONCATALYTIC ) ||
        (config->GetMarker_All_KindBC(iMarker) == ISOTHERMAL             ) ||
        (config->GetMarker_All_KindBC(iMarker) == ISOTHERMAL_CATALYTIC   ) ||
        (config->GetMarker_All_KindBC(iMarker) == ISOTHERMAL_NONCATALYTIC)) {
      for(iVertex = 0; iVertex<geo_coarse->nVertex[iMarker]; iVertex++) {
        Point_Coarse = geo_coarse->vertex[iMarker][iVertex]->GetNode();
        sol_coarse->node[Point_Coarse]->SetVel_ResTruncError_Zero();
      }
    }
  }
  
  delete [] Residual;
}

void CMultiGridIntegration::SetRestricted_Solution(unsigned short RunTime_EqSystem, CSolver *sol_fine, CSolver *sol_coarse, CGeometry *geo_fine, CGeometry *geo_coarse, CConfig *config) {
  unsigned long iVertex, Point_Fine, Point_Coarse;
  unsigned short iMarker, iVar, iChildren, iDim;
  double Area_Parent, Area_Children, *Solution_Fine, *Grid_Vel, Vector[3];
  
  const unsigned short SolContainer_Position = config->GetContainerPosition(RunTime_EqSystem);
  const unsigned short nVar = sol_coarse->GetnVar();
  const unsigned short nDim = geo_fine->GetnDim();
  const bool grid_movement  = config->GetGrid_Movement();
  
  double *Solution = new double[nVar];
  
  /*--- Compute coarse solution from fine solution ---*/
  for (Point_Coarse = 0; Point_Coarse < geo_coarse->GetnPointDomain(); Point_Coarse++) {
    Area_Parent = geo_coarse->node[Point_Coarse]->GetVolume();
    
    for (iVar = 0; iVar < nVar; iVar++) Solution[iVar] = 0.0;
    
    for (iChildren = 0; iChildren < geo_coarse->node[Point_Coarse]->GetnChildren_CV(); iChildren++) {
      
      Point_Fine = geo_coarse->node[Point_Coarse]->GetChildren_CV(iChildren);
      Area_Children = geo_fine->node[Point_Fine]->GetVolume();
      Solution_Fine = sol_fine->node[Point_Fine]->GetSolution();
      for (iVar = 0; iVar < nVar; iVar++) {
        Solution[iVar] += Solution_Fine[iVar]*Area_Children/Area_Parent;
      }
    }
    
    sol_coarse->node[Point_Coarse]->SetSolution(Solution);
    
  }
  
  /*--- Update the solution at the no-slip walls ---*/
  for (iMarker = 0; iMarker < config->GetnMarker_All(); iMarker++) {
    if ((config->GetMarker_All_KindBC(iMarker) == HEAT_FLUX              ) ||
        (config->GetMarker_All_KindBC(iMarker) == HEAT_FLUX_CATALYTIC    ) ||
        (config->GetMarker_All_KindBC(iMarker) == HEAT_FLUX_NONCATALYTIC ) ||
        (config->GetMarker_All_KindBC(iMarker) == ISOTHERMAL             ) ||
        (config->GetMarker_All_KindBC(iMarker) == ISOTHERMAL_CATALYTIC   ) ||
        (config->GetMarker_All_KindBC(iMarker) == ISOTHERMAL_NONCATALYTIC)) {
      for (iVertex = 0; iVertex < geo_coarse->nVertex[iMarker]; iVertex++) {
        Point_Coarse = geo_coarse->vertex[iMarker][iVertex]->GetNode();
        
        if (SolContainer_Position == FLOW_SOL) {
          /*--- At moving walls, set the solution based on the new density and wall velocity ---*/
          if (grid_movement) {
            Grid_Vel = geo_coarse->node[Point_Coarse]->GetGridVel();
            for (iDim = 0; iDim < nDim; iDim++)
              Vector[iDim] = sol_coarse->node[Point_Coarse]->GetSolution(0)*Grid_Vel[iDim];
            sol_coarse->node[Point_Coarse]->SetVelSolutionVector(Vector);
          } else {
            /*--- For stationary no-slip walls, set the velocity to zero. ---*/
            sol_coarse->node[Point_Coarse]->SetVelSolutionZero();
          }
        }
        if (SolContainer_Position == ADJFLOW_SOL) {
          sol_coarse->node[Point_Coarse]->SetVelSolutionDVector();
        }
        
      }
    }
  }
  
  delete [] Solution;
  
}

void CMultiGridIntegration::SetRestricted_Gradient(unsigned short RunTime_EqSystem, CSolver **sol_fine, CSolver **sol_coarse, CGeometry *geo_fine,
                                                   CGeometry *geo_coarse, CConfig *config) {
  unsigned long Point_Fine, Point_Coarse;
  unsigned short iVar, iDim, iChildren;
  double Area_Parent, Area_Children, **Gradient_fine;
  
  const unsigned short SolContainer_Position = config->GetContainerPosition(RunTime_EqSystem);
  const unsigned short nDim = geo_coarse->GetnDim();
  const unsigned short nVar = sol_coarse[SolContainer_Position]->GetnVar();
  
  double **Gradient = new double* [nVar];
  for (iVar = 0; iVar < nVar; iVar++)
    Gradient[iVar] = new double [nDim];
  
  for (Point_Coarse = 0; Point_Coarse < geo_coarse->GetnPoint(); Point_Coarse++) {
    Area_Parent = geo_coarse->node[Point_Coarse]->GetVolume();
    
    for (iVar = 0; iVar < nVar; iVar++)
      for (iDim = 0; iDim < nDim; iDim++)
        Gradient[iVar][iDim] = 0.0;
    
    for (iChildren = 0; iChildren < geo_coarse->node[Point_Coarse]->GetnChildren_CV(); iChildren++) {
      Point_Fine = geo_coarse->node[Point_Coarse]->GetChildren_CV(iChildren);
      Area_Children = geo_fine->node[Point_Fine]->GetVolume();
      Gradient_fine = sol_fine[SolContainer_Position]->node[Point_Fine]->GetGradient();
      
      for (iVar = 0; iVar < nVar; iVar++)
        for (iDim = 0; iDim < nDim; iDim++)
          Gradient[iVar][iDim] += Gradient_fine[iVar][iDim]*Area_Children/Area_Parent;
    }
    sol_coarse[SolContainer_Position]->node[Point_Coarse]->SetGradient(Gradient);
  }
  
  for (iVar = 0; iVar < nVar; iVar++)
    delete [] Gradient[iVar];
  delete [] Gradient;
  
}

void CMultiGridIntegration::NonDimensional_Parameters(CGeometry **geometry, CSolver ***solver_container, CNumerics ****numerics_container,
                                                      CConfig *config, unsigned short FinestMesh, unsigned short RunTime_EqSystem, unsigned long Iteration,
                                                      double *monitor) {
  
  switch (RunTime_EqSystem) {
      
    case RUNTIME_FLOW_SYS:
      
      /*--- Calculate the inviscid and viscous forces ---*/
      solver_container[FinestMesh][FLOW_SOL]->Inviscid_Forces(geometry[FinestMesh], config);
      if (config->GetKind_ViscNumScheme() != NONE) solver_container[FinestMesh][FLOW_SOL]->Viscous_Forces(geometry[FinestMesh], config);
      
      /*--- Evaluate convergence monitor ---*/
      if (config->GetConvCriteria() == CAUCHY) {
        if (config->GetCauchy_Func_Flow() == DRAG_COEFFICIENT) (*monitor) = solver_container[FinestMesh][FLOW_SOL]->GetTotal_CDrag();
        if (config->GetCauchy_Func_Flow() == LIFT_COEFFICIENT) (*monitor) = solver_container[FinestMesh][FLOW_SOL]->GetTotal_CLift();
        if (config->GetCauchy_Func_Flow() == NEARFIELD_PRESSURE) (*monitor) = solver_container[FinestMesh][FLOW_SOL]->GetTotal_CNearFieldOF();
      }
      
      if (config->GetConvCriteria() == RESIDUAL)
        (*monitor) = log10(solver_container[FinestMesh][FLOW_SOL]->GetRes_RMS(0));
      
      break;
      
    case RUNTIME_ADJFLOW_SYS:
      
      /*--- Calculate the inviscid and viscous sensitivities ---*/
      solver_container[FinestMesh][ADJFLOW_SOL]->Inviscid_Sensitivity(geometry[FinestMesh], solver_container[FinestMesh], numerics_container[FinestMesh][ADJFLOW_SOL][CONV_BOUND_TERM], config);
      if (config->GetKind_ViscNumScheme() != NONE) solver_container[FinestMesh][ADJFLOW_SOL]->Viscous_Sensitivity(geometry[FinestMesh], solver_container[FinestMesh], numerics_container[FinestMesh][ADJFLOW_SOL][CONV_BOUND_TERM], config);
      
      /*--- Smooth the inviscid and viscous sensitivities ---*/
      if (config->GetKind_SensSmooth() != NONE) solver_container[FinestMesh][ADJFLOW_SOL]->Smooth_Sensitivity(geometry[FinestMesh], solver_container[FinestMesh], numerics_container[FinestMesh][ADJFLOW_SOL][CONV_BOUND_TERM], config);
      
      /*--- Evaluate convergence monitor ---*/
      if (config->GetConvCriteria() == CAUCHY) {
        if (config->GetCauchy_Func_AdjFlow() == SENS_GEOMETRY) (*monitor) = solver_container[FinestMesh][ADJFLOW_SOL]->GetTotal_Sens_Geo();
        if (config->GetCauchy_Func_AdjFlow() == SENS_MACH) (*monitor) = solver_container[FinestMesh][ADJFLOW_SOL]->GetTotal_Sens_Mach();
      }
      if (config->GetConvCriteria() == RESIDUAL)
        (*monitor) = log10(solver_container[FinestMesh][ADJFLOW_SOL]->GetRes_RMS(0));
      
      break;
      
    case RUNTIME_TNE2_SYS:
      
      /*--- Calculate the inviscid and viscous forces ---*/
      solver_container[FinestMesh][TNE2_SOL]->Inviscid_Forces(geometry[FinestMesh], config);
      if ((config->GetKind_Solver() == TNE2_NAVIER_STOKES) && (config->GetKind_ViscNumScheme() != NONE))
        solver_container[FinestMesh][TNE2_SOL]->Viscous_Forces(geometry[FinestMesh], config);
      
      /*--- Evaluate convergence monitor ---*/
      if (config->GetConvCriteria() == CAUCHY) {
        if (config->GetCauchy_Func_Flow() == DRAG_COEFFICIENT) (*monitor) = solver_container[FinestMesh][TNE2_SOL]->GetTotal_CDrag();
        if (config->GetCauchy_Func_Flow() == LIFT_COEFFICIENT) (*monitor) = solver_container[FinestMesh][TNE2_SOL]->GetTotal_CLift();
      }
      
      if (config->GetConvCriteria() == RESIDUAL)
        (*monitor) = log10(solver_container[FinestMesh][TNE2_SOL]->GetRes_RMS(0));
      
      break;
      
    case RUNTIME_ADJTNE2_SYS:
      
      /*--- Calculate the inviscid and viscous sensitivities ---*/
      solver_container[FinestMesh][ADJTNE2_SOL]->Inviscid_Sensitivity(geometry[FinestMesh], solver_container[FinestMesh], numerics_container[FinestMesh][ADJTNE2_SOL][CONV_BOUND_TERM], config);
      if (config->GetKind_ViscNumScheme() != NONE) solver_container[FinestMesh][ADJTNE2_SOL]->Viscous_Sensitivity(geometry[FinestMesh], solver_container[FinestMesh], numerics_container[FinestMesh][ADJTNE2_SOL][CONV_BOUND_TERM], config);
      
      /*--- Smooth the inviscid and viscous sensitivities ---*/
      if (config->GetKind_SensSmooth() != NONE) solver_container[FinestMesh][ADJTNE2_SOL]->Smooth_Sensitivity(geometry[FinestMesh], solver_container[FinestMesh], numerics_container[FinestMesh][ADJTNE2_SOL][CONV_BOUND_TERM], config);
      
      /*--- Evaluate convergence monitor ---*/
      if (config->GetConvCriteria() == CAUCHY) {
        if (config->GetCauchy_Func_AdjFlow() == SENS_GEOMETRY) (*monitor) = solver_container[FinestMesh][ADJTNE2_SOL]->GetTotal_Sens_Geo();
        if (config->GetCauchy_Func_AdjFlow() == SENS_MACH) (*monitor) = solver_container[FinestMesh][ADJTNE2_SOL]->GetTotal_Sens_Mach();
      }
      if (config->GetConvCriteria() == RESIDUAL)
        (*monitor) = log10(solver_container[FinestMesh][ADJTNE2_SOL]->GetRes_RMS(0));
      
      break;
      
    case RUNTIME_LINFLOW_SYS:
      
      /*--- Calculate the inviscid and viscous forces ---*/
      solver_container[FinestMesh][LINFLOW_SOL]->Inviscid_DeltaForces(geometry[FinestMesh], solver_container[FinestMesh], config);
      if (config->GetKind_ViscNumScheme() != NONE) solver_container[FinestMesh][LINFLOW_SOL]->Viscous_DeltaForces(geometry[FinestMesh], config);
      
      /*--- Evaluate convergence monitor ---*/
      if (config->GetConvCriteria() == CAUCHY) {
        if (config->GetCauchy_Func_LinFlow() == DELTA_DRAG_COEFFICIENT) (*monitor) = solver_container[FinestMesh][LINFLOW_SOL]->GetTotal_CDeltaDrag();
        if (config->GetCauchy_Func_LinFlow() == DELTA_LIFT_COEFFICIENT) (*monitor) = solver_container[FinestMesh][LINFLOW_SOL]->GetTotal_CDeltaLift();
      }
      if (config->GetConvCriteria() == RESIDUAL)
        (*monitor) = log10(solver_container[FinestMesh][LINFLOW_SOL]->GetRes_RMS(0));
      
      break;
  }
}

CSingleGridIntegration::CSingleGridIntegration(CConfig *config) : CIntegration(config) { }

CSingleGridIntegration::~CSingleGridIntegration(void) { }

void CSingleGridIntegration::SingleGrid_Iteration(CGeometry **geometry, CSolver ***solver_container,
                                                  CNumerics ****numerics_container, CConfig *config, unsigned short RunTime_EqSystem, unsigned long Iteration) {
  unsigned short iMesh;
  double monitor = 0.0;
  
  unsigned short SolContainer_Position = config->GetContainerPosition(RunTime_EqSystem);
  
  /*--- Preprocessing ---*/
  
  solver_container[MESH_0][SolContainer_Position]->Preprocessing(geometry[MESH_0], solver_container[MESH_0], config, MESH_0, 0, RunTime_EqSystem, false);
  
  /*--- Set the old solution ---*/
  
  solver_container[MESH_0][SolContainer_Position]->Set_OldSolution(geometry[MESH_0]);
  
  /*--- Time step evaluation ---*/
  
  solver_container[MESH_0][SolContainer_Position]->SetTime_Step(geometry[MESH_0], solver_container[MESH_0], config, MESH_0, 0);
  
  /*--- Space integration ---*/
  
  Space_Integration(geometry[MESH_0], solver_container[MESH_0], numerics_container[MESH_0][SolContainer_Position],
                    config, MESH_0, NO_RK_ITER, RunTime_EqSystem);
  
  /*--- Time integration ---*/
  
  Time_Integration(geometry, solver_container, config, NO_RK_ITER,
                   RunTime_EqSystem, Iteration, MESH_0);
  
  /*--- Postprocessing ---*/
  
  solver_container[MESH_0][SolContainer_Position]->Postprocessing(geometry[MESH_0], solver_container[MESH_0], config, MESH_0);
  
  /*--- Compute adimensional parameters and the convergence monitor ---*/
  
  switch (RunTime_EqSystem) {
    case RUNTIME_WAVE_SYS:    monitor = log10(solver_container[MESH_0][WAVE_SOL]->GetRes_RMS(0));    break;
    case RUNTIME_FEA_SYS:     monitor = log10(solver_container[MESH_0][FEA_SOL]->GetRes_RMS(0));     break;
    case RUNTIME_HEAT_SYS:    monitor = log10(solver_container[MESH_0][HEAT_SOL]->GetRes_RMS(0));    break;
    case RUNTIME_POISSON_SYS: monitor = log10(solver_container[MESH_0][POISSON_SOL]->GetRes_RMS(0)); break;
  }
  
  /*--- Convergence strategy ---*/
  
  Convergence_Monitoring(geometry[MESH_0], config, Iteration, monitor);
  
  /*--- Copy the solution to the coarse levels, and run the post-processing ---*/
  
  for (iMesh = 0; iMesh < config->GetMGLevels(); iMesh++) {
    SetRestricted_Solution(RunTime_EqSystem, solver_container[iMesh], solver_container[iMesh+1],
                           geometry[iMesh], geometry[iMesh+1], config);
    
    solver_container[iMesh+1][SolContainer_Position]->Postprocessing(geometry[iMesh+1],
                                                                            solver_container[iMesh+1], config, iMesh+1);
  }
  
}

void CSingleGridIntegration::SetRestricted_Solution(unsigned short RunTime_EqSystem, CSolver **sol_fine, CSolver **sol_coarse, CGeometry *geo_fine, CGeometry *geo_coarse, CConfig *config) {
  unsigned long iVertex, Point_Fine, Point_Coarse;
  unsigned short iMarker, iVar, iChildren;
  double Area_Parent, Area_Children, *Solution_Fine, *Solution;
  
  unsigned short SolContainer_Position = config->GetContainerPosition(RunTime_EqSystem);
  unsigned short nVar = sol_coarse[SolContainer_Position]->GetnVar();
  
  Solution = new double[nVar];
  
  /*--- Compute coarse solution from fine solution ---*/
  
  for (Point_Coarse = 0; Point_Coarse < geo_coarse->GetnPointDomain(); Point_Coarse++) {
    Area_Parent = geo_coarse->node[Point_Coarse]->GetVolume();
    
    for (iVar = 0; iVar < nVar; iVar++) Solution[iVar] = 0.0;
    
    for (iChildren = 0; iChildren < geo_coarse->node[Point_Coarse]->GetnChildren_CV(); iChildren++) {
      
      Point_Fine = geo_coarse->node[Point_Coarse]->GetChildren_CV(iChildren);
      Area_Children = geo_fine->node[Point_Fine]->GetVolume();
      Solution_Fine = sol_fine[SolContainer_Position]->node[Point_Fine]->GetSolution();
      for (iVar = 0; iVar < nVar; iVar++)
        Solution[iVar] += Solution_Fine[iVar]*Area_Children/Area_Parent;
    }
    
    sol_coarse[SolContainer_Position]->node[Point_Coarse]->SetSolution(Solution);
    
  }
    
  /*--- Update solution at the no slip wall boundary, only the first 
   variable (nu_tilde -in SA- and k -in SST-), to guarantee that the eddy viscoisty 
   is zero on the surface ---*/
  
  if (RunTime_EqSystem == RUNTIME_TURB_SYS) {
    for (iMarker = 0; iMarker < config->GetnMarker_All(); iMarker++) {
      if ((config->GetMarker_All_KindBC(iMarker) == HEAT_FLUX              ) ||
          (config->GetMarker_All_KindBC(iMarker) == HEAT_FLUX_CATALYTIC    ) ||
          (config->GetMarker_All_KindBC(iMarker) == HEAT_FLUX_NONCATALYTIC ) ||
          (config->GetMarker_All_KindBC(iMarker) == ISOTHERMAL             ) ||
          (config->GetMarker_All_KindBC(iMarker) == ISOTHERMAL_CATALYTIC   ) ||
          (config->GetMarker_All_KindBC(iMarker) == ISOTHERMAL_NONCATALYTIC)) {
        for (iVertex = 0; iVertex < geo_coarse->nVertex[iMarker]; iVertex++) {
          Point_Coarse = geo_coarse->vertex[iMarker][iVertex]->GetNode();
          sol_coarse[SolContainer_Position]->node[Point_Coarse]->SetSolutionZero(0);
        }
      }
    }
  }
  
  delete [] Solution;
  
}
