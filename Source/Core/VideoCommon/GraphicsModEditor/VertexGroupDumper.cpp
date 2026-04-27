// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "VideoCommon/GraphicsModEditor/VertexGroupDumper.h"

#include <algorithm>
#include <future>
#include <queue>
#include <random>
#include <set>
#include <utility>

#include <Eigen/Core>
#include <Eigen/Dense>
#include <Eigen/Geometry>
#include <Eigen/Sparse>
#include <Eigen/SparseCholesky>
#include <igl/remove_duplicate_vertices.h>

#include <fmt/chrono.h>
#include <fmt/format.h>
#include <tinygltf/tiny_gltf.h>

#include "Common/FileUtil.h"
#include "Common/IOFile.h"
#include "Common/JsonUtil.h"
#include "Common/Logging/Log.h"
#include "Common/TimeUtil.h"

#include "Core/ConfigManager.h"

#include "VideoCommon/GraphicsModEditor/GpuSkinningDataUtils.h"

#ifdef _MSC_VER
#pragma warning(disable : 4267)  // "conversion from size_t to int in libigl
#endif

#include <Eigen/Dense>
#include <iostream>
#include <map>
#include <vector>

struct ReferenceBone
{
  std::string name;
  std::vector<int> vertIndices;
  double maxMDE = 0.0;
};

using DrawToSkinningIdToGlobalBone = std::map<GraphicsModSystem::DrawCallID, std::map<int, int>>;

namespace
{
namespace SimpleGeodesic
{

struct HeatGeodesicsData
{
  Eigen::SparseMatrix<double> L;  // Laplacian
  Eigen::SparseMatrix<double> M;  // Mass Matrix
  Eigen::SimplicialLLT<Eigen::SparseMatrix<double>> heat_solver;
  Eigen::MatrixXd V;
  Eigen::MatrixXi F;
  double t;  // Time step
  Eigen::VectorXd geo_from_center;
};

int FindAnatomicalSource(const Eigen::MatrixXd& V)
{
  Eigen::Vector3d centroid = V.colwise().mean();
  int best_v = 0;
  double min_dist = 1e12;

  for (int i = 0; i < V.rows(); ++i)
  {
    double d = (V.row(i).transpose() - centroid).norm();
    if (d < min_dist)
    {
      min_dist = d;
      best_v = i;
    }
  }
  return best_v;  // This is the "Mass Center" vertex
}

bool heat_geodesics_precompute(const Eigen::MatrixXd& V, const Eigen::MatrixXi& F,
                               SimpleGeodesic::HeatGeodesicsData& data)
{
  int n = (int)V.rows();
  data.V = V;
  data.F = F;

  // 1. Standard Cotangent Laplacian (L) and Mass (M)
  std::vector<Eigen::Triplet<double>> L_triplets, M_triplets;
  for (int f = 0; f < F.rows(); ++f)
  {
    for (int i = 0; i < 3; ++i)
    {
      int i0 = F(f, i), i1 = F(f, (i + 1) % 3), i2 = F(f, (i + 2) % 3);
      Eigen::Vector3d v0 = V.row(i0), v1 = V.row(i1), v2 = V.row(i2);
      Eigen::Vector3d e1 = v1 - v0, e2 = v2 - v0, e3 = v2 - v1;

      double area = 0.5 * e1.cross(e2).norm();
      if (area < 1e-10)
        continue;

      // Cotangents
      double cot0 = e1.dot(e2) / e1.cross(e2).norm();
      double weight = 0.5 * cot0;

      L_triplets.push_back({i1, i2, weight});
      L_triplets.push_back({i2, i1, weight});
      L_triplets.push_back({i1, i1, -weight});
      L_triplets.push_back({i2, i2, -weight});
      M_triplets.push_back({i0, i0, area / 3.0});
    }
  }

  Eigen::SparseMatrix<double> L(n, n), M(n, n);
  L.setFromTriplets(L_triplets.begin(), L_triplets.end());
  M.setFromTriplets(M_triplets.begin(), M_triplets.end());

  // 2. ROBUST TIME STEP (t)
  // Instead of mean edge, use the bounding box diagonal for stability
  Eigen::Vector3d minB = V.colwise().minCoeff();
  Eigen::Vector3d maxB = V.colwise().maxCoeff();
  double h = (maxB - minB).norm() / 20.0;  // Typical heuristic
  data.t = h * h;

  // 3. SOLVE HEAT: (M - tL)u = delta
  Eigen::SparseMatrix<double> A_heat = M - data.t * L;

  // Add a tiny stabilizer to the diagonal to handle non-manifold/floating verts
  for (int i = 0; i < n; ++i)
    A_heat.coeffRef(i, i) += 1e-8;

  data.heat_solver.compute(A_heat);

  Eigen::VectorXd rhs = Eigen::VectorXd::Zero(n);
  rhs(FindAnatomicalSource(V)) = 1.0;
  Eigen::VectorXd u = data.heat_solver.solve(rhs);

  // 5. TRANSFORM TO DISTANCE
  Eigen::VectorXd d(n);
  for (int i = 0; i < n; ++i)
  {
    // Correcting the Varadhan's approximation: distance ~ -sqrt(t) * log(u)
    d(i) = -std::sqrt(data.t) * std::log(std::max(1e-12, u(i)));
  }

  // Normalize
  double minv = d.minCoeff();
  d.array() -= minv;
  double maxv = d.maxCoeff();
  if (maxv > 1e-12)
    d /= maxv;

  data.geo_from_center = d;
  data.L = L;
  data.M = M;
  return data.heat_solver.info() == Eigen::Success;
}

}  // namespace SimpleGeodesic

/**
 * @brief Merges vertices within a 3D Euclidean distance (L2) of each other.
 * Unlike grid-based (L1) welders, this ensures diagonal gaps are bridged.
 *
 * @param V Initial Nx3 vertex matrix (raw stitched chunks).
 * @param F Initial Mx3 face matrix (global soup).
 * @param epsilon The max distance between two vertices to be considered "identical".
 * @param SV [Output] Unique vertex positions (the "Survivors").
 * @param SVI [Output] Map: Unique Index -> Original Index (first representative).
 * @param SVJ [Output] Map: Original Index -> Unique Index (the "Zipper" map).
 * @param SF [Output] Updated face indices pointing to the new unique vertices.
 */
void weld_mesh_euclidean(const Eigen::MatrixXd& V,  // Input: N x 3 (original vertices)
                         const Eigen::MatrixXi& F,  // Input: M x 3 (original faces)
                         double epsilon,            // Input: Threshold (e.g., 0.001)
                         Eigen::MatrixXd& SV,       // Output: Unique Vertices
                         Eigen::VectorXi& SVI,      // Output: Surviving Indices
                         Eigen::VectorXi& SVJ,      // Output: Mapping (Old -> New)
                         Eigen::MatrixXi& SF)       // Output: Updated Faces
{
  const int N = static_cast<int>(V.rows());
  SVJ.setConstant(N, -1);
  std::vector<int> survivors;
  std::vector<Eigen::Vector3d> unique_list;

  // 1. Identify Unique Vertices based on actual L2 distance
  for (int i = 0; i < N; ++i)
  {
    Eigen::Vector3d p = V.row(i);
    int match = -1;

    // Linear search is fine for 2k-10k vertices.
    // TODO: use spatial hash to optimize
    for (int j = 0; j < (int)unique_list.size(); ++j)
    {
      if ((p - unique_list[j]).norm() <= epsilon)
      {
        match = j;
        break;
      }
    }

    if (match == -1)
    {
      SVJ(i) = static_cast<int>(unique_list.size());
      unique_list.push_back(p);
      survivors.push_back(i);
    }
    else
    {
      SVJ(i) = match;
    }
  }

  // 2. Build SV (Unique Vertices)
  SV.resize(unique_list.size(), 3);
  for (int i = 0; i < (int)unique_list.size(); ++i)
  {
    SV.row(i) = unique_list[i];
  }

  // 3. Build SVI (The first index that represented this unique spot)
  SVI.resize(survivors.size());
  for (int i = 0; i < (int)survivors.size(); ++i)
  {
    SVI(i) = survivors[i];
  }

  // 4. Build SF (The "Zippered" Faces)
  SF.resize(F.rows(), 3);
  for (int i = 0; i < F.rows(); ++i)
  {
    for (int j = 0; j < 3; ++j)
    {
      SF(i, j) = SVJ(F(i, j));
    }
  }
}

Common::Vec3 ReadPosition(const u8* vert_ptr, u32 position_offset)
{
  Common::Vec3 vertex_position;
  std::memcpy(&vertex_position.x, vert_ptr + position_offset, sizeof(float));
  std::memcpy(&vertex_position.y, vert_ptr + sizeof(float) + position_offset, sizeof(float));
  std::memcpy(&vertex_position.z, vert_ptr + sizeof(float) * 2 + position_offset, sizeof(float));
  return vertex_position;
}

std::vector<Common::Vec3> ReadMeshPositions(const GraphicsModSystem::DrawDataView& draw_data)
{
  std::vector<Common::Vec3> result;
  for (u32 i = 0; i < draw_data.vertex_count; i++)
  {
    const auto pos =
        ReadPosition(draw_data.vertex_data + i * draw_data.vertex_format->GetVertexStride(),
                     draw_data.vertex_format->GetVertexDeclaration().position.offset);
    result.push_back(pos);
  }
  return result;
}

Eigen::Matrix3Xd ConvertMeshPositions(const std::vector<Common::Vec3>& positions,
                                      const GraphicsModSystem::DrawDataView& draw_data)
{
  const auto& declaration = draw_data.vertex_format->GetVertexDeclaration();

  Common::Matrix44 position_transform;
  if (!declaration.posmtx.enable)
  {
    Common::Matrix44 object_transform;
    for (std::size_t i = 0; i < 3; i++)
    {
      object_transform.data[i * 4 + 0] = draw_data.object_transform[i][0];
      object_transform.data[i * 4 + 1] = draw_data.object_transform[i][1];
      object_transform.data[i * 4 + 2] = draw_data.object_transform[i][2];
      object_transform.data[i * 4 + 3] = draw_data.object_transform[i][3];
    }
    object_transform.data[12] = 0;
    object_transform.data[13] = 0;
    object_transform.data[14] = 0;
    object_transform.data[15] = 1;
    position_transform = object_transform;
  }

  Eigen::Matrix3Xd result(3, positions.size());
  for (std::size_t i = 0; i < positions.size(); ++i)
  {
    if (declaration.posmtx.enable)
    {
      // Position transform is actually vertex specific
      u32 gpu_skin_index;
      std::memcpy(&gpu_skin_index,
                  draw_data.vertex_data + i * declaration.stride + declaration.posmtx.offset,
                  sizeof(u32));

      for (std::size_t j = 0; j < 3; j++)
      {
        position_transform.data[j * 4 + 0] =
            draw_data.gpu_skinning_position_transform[gpu_skin_index + j][0];
        position_transform.data[j * 4 + 1] =
            draw_data.gpu_skinning_position_transform[gpu_skin_index + j][1];
        position_transform.data[j * 4 + 2] =
            draw_data.gpu_skinning_position_transform[gpu_skin_index + j][2];
        position_transform.data[j * 4 + 3] =
            draw_data.gpu_skinning_position_transform[gpu_skin_index + j][3];
      }
      position_transform.data[12] = 0;
      position_transform.data[13] = 0;
      position_transform.data[14] = 0;
      position_transform.data[15] = 1;
    }
    const auto pos = positions[i];
    const auto pos_object_space = position_transform.Transform(pos, 1);
    result.col(i) = Eigen::Vector3d(pos_object_space.x, pos_object_space.y, pos_object_space.z);
  }

  return result;
}

std::vector<int> GetGPUSkinningIDs(const GraphicsModSystem::DrawDataView& draw_data)
{
  const auto& declaration = draw_data.vertex_format->GetVertexDeclaration();

  if (!declaration.posmtx.enable)
    return {};

  std::vector<int> result;
  for (std::size_t i = 0; i < draw_data.vertex_count; i++)
  {
    // Position transform is actually vertex specific
    u32 gpu_skin_index;
    std::memcpy(&gpu_skin_index,
                draw_data.vertex_data + i * declaration.stride + declaration.posmtx.offset,
                sizeof(u32));
    result.push_back(static_cast<int>(gpu_skin_index));
  }

  return result;
}

bool IsDuplicateMesh(const Eigen::Matrix3Xd& mesh,
                     const std::vector<Eigen::Matrix3Xd>& existing_meshes, float epsilon = 1e-4f)
{
  for (const auto& existing_mesh : existing_meshes)
  {
    Eigen::Matrix3Xd diff = (mesh - existing_mesh).cwiseAbs();

    if (diff.maxCoeff() < epsilon)
    {
      return true;
    }
  }
  return false;
}

/**
 * @brief Combines individual 3xN chunks into one 3xTotal global matrix.
 * @param chunks A vector of matrices, one for each capture chunk.
 * @return A single Eigen::Matrix3Xd representing the full captured pose.
 */
Eigen::Matrix3Xd StitchChunks(const std::vector<Eigen::Matrix3Xd>& chunks)
{
  // 1. Calculate total vertex count
  int total_vertices = 0;
  for (const auto& chunk : chunks)
  {
    total_vertices += (int)chunk.cols();
  }

  // 2. Pre-allocate the global matrix [3 rows x Total Cols]
  Eigen::Matrix3Xd V_global(3, total_vertices);

  // 3. Fill the global matrix chunk-by-chunk
  int current_offset = 0;
  for (const auto& chunk : chunks)
  {
    // block(start_row, start_col, num_rows, num_cols)
    V_global.block(0, current_offset, 3, chunk.cols()) = chunk;
    current_offset += (int)chunk.cols();
  }

  return V_global;
}

double FindMinimumChunkGap(const Eigen::Matrix3Xd& restPose,
                           const std::vector<GraphicsModSystem::DrawCallID>& vertex_per_draw_call)
{
  const int N = static_cast<int>(restPose.cols());
  double minTrueGap = 1e9;
  bool foundAnyGap = false;

  for (int i = 0; i < N; ++i)
  {
    for (int j = i + 1; j < N; ++j)
    {
      if (vertex_per_draw_call[i] != vertex_per_draw_call[j])
      {
        double d = (restPose.col(i) - restPose.col(j)).norm();

        // IGNORE exact overlaps (already handled or noise)
        // We want the smallest REAL distance between separate vertices
        if (d > 1e-7 && d < minTrueGap)
        {
          minTrueGap = d;
          foundAnyGap = true;
        }
      }
    }
  }

  // If we found a real gap (like your 0.00023),
  // add a 20% buffer to ensure the weld catches it.
  if (foundAnyGap)
    return minTrueGap * 1.2;

  // Fallback: If everything was 0, use a standard safe epsilon
  return 1e-4;
}

std::vector<Eigen::Matrix3Xd> ReassembleSignificantFrames(
    const std::map<GraphicsModSystem::DrawCallID, GraphicsModEditor::VertexGroupDumper::FinalData>&
        draw_call_to_mesh_data)
{
  std::set<int> frames_with_changes;
  std::vector<Eigen::Matrix3Xd> lastKnownPoses;
  for (const auto& [draw_call, data] : draw_call_to_mesh_data)
  {
    for (auto const& [frame_id, mesh_index] : data.frame_id_to_mesh_index)
    {
      frames_with_changes.insert(frame_id);
    }

    // Initialize with our rest pose (first frame)
    lastKnownPoses.push_back(data.mesh_poses[0]);
  }

  std::vector<Eigen::Matrix3Xd> motionFrames;
  for (int frame_id : frames_with_changes)
  {
    std::size_t draw_call_count = 0;
    for (const auto& [draw_call, data] : draw_call_to_mesh_data)
    {
      if (auto it = data.frame_id_to_mesh_index.find(frame_id);
          it != data.frame_id_to_mesh_index.end())
      {
        lastKnownPoses[draw_call_count] = data.mesh_poses[it->second];
      }
      draw_call_count++;
    }
    // This frame now contains the most recent data for every chunk
    motionFrames.push_back(StitchChunks(lastKnownPoses));
  }

  return motionFrames;
}

std::vector<int> GetGlobalGPUSkinningIDs(
    const std::map<GraphicsModSystem::DrawCallID, GraphicsModEditor::VertexGroupDumper::FinalData>&
        draw_call_to_mesh_data,
    DrawToSkinningIdToGlobalBone* draw_to_skinning_details)
{
  std::size_t total_vertices = 0;
  for (const auto& [draw_call, data] : draw_call_to_mesh_data)
  {
    total_vertices += static_cast<std::size_t>(data.mesh_poses[0].cols());
  }

  std::vector<int> gpu_skinning_ids(total_vertices, -1);
  int next_global_id = 0;

  int current_offset = 0;
  for (const auto& [draw_call, data] : draw_call_to_mesh_data)
  {
    const auto vertex_count = static_cast<std::size_t>(data.mesh_poses[0].cols());

    auto& skinning_details = (*draw_to_skinning_details)[draw_call];

    for (std::size_t i = 0; i < data.gpu_skinning_ids.size(); i++)
    {
      const int gpu_skinning_id = data.gpu_skinning_ids[i];
      const auto [iter, added] = skinning_details.try_emplace(gpu_skinning_id, 0);
      if (added)
      {
        iter->second = next_global_id;
        next_global_id++;
      }
      gpu_skinning_ids[current_offset + i] = iter->second;
    }

    current_offset += vertex_count;
  }

  return gpu_skinning_ids;
}

/**
 * Normalizes an animation sequence after it has been welded.
 * 1. Centers Pose 0 at the origin.
 * 2. Scales based on Pose 0 bounding box.
 * 3. Aligns all frames to Pose 0.
 */
void NormalizeWeldedAnimation(std::vector<Eigen::Matrix3Xd>& poses, double& outScale,
                              Eigen::Vector3d& centroid)
{
  if (poses.empty())
    return;

  // A. Center and Scale based on Pose 0
  centroid = poses[0].rowwise().mean();
  for (auto& p : poses)
    p.colwise() -= centroid;

  Eigen::Vector3d minB = poses[0].rowwise().minCoeff();
  Eigen::Vector3d maxB = poses[0].rowwise().maxCoeff();
  outScale = (maxB - minB).norm();

  if (outScale > 1e-8)
  {
    for (auto& p : poses)
      p /= outScale;
  }

  // B. Global Alignment
  // We align everything to poses[0] (which is now centered/scaled)
  const Eigen::Matrix3Xd& reference = poses[0];

  for (size_t k = 1; k < poses.size(); ++k)
  {
    Eigen::Matrix3Xd& currentPose = poses[k];

    // 1. Center the current frame locally
    Eigen::Vector3d currentCentroid = currentPose.rowwise().mean();
    Eigen::Matrix3Xd centeredCurrent = currentPose.colwise() - currentCentroid;

    // 2. Solve for Rotation (Kabsch)
    // Note: Reference is already at origin, so we use reference directly
    Eigen::Matrix3d H = centeredCurrent * reference.transpose();

    Eigen::JacobiSVD<Eigen::Matrix3d> svd(H, Eigen::ComputeFullU | Eigen::ComputeFullV);
    Eigen::Matrix3d R = svd.matrixV() * svd.matrixU().transpose();

    if (R.determinant() < 0)
    {
      Eigen::Matrix3d V = svd.matrixV();
      V.col(2) *= -1;
      R = V * svd.matrixU().transpose();
    }

    // 3. Update the pose: Remove its translation, rotate it,
    // and keep it at the origin to match reference.
    currentPose = R * centeredCurrent;
  }
}

/**
 * Solves the "Procrustes Problem" (Kabsch Algorithm) for a set of points.
 *
 * Given a group of vertices in their 'Rest Pose' and 'Animated Pose',
 * this function finds the single best Rotation (R) and Translation (T)
 * that aligns the rest points to the animated ones.
 *
 * It calculates the 'Sum of Squared Errors' (SSE) in World Space.
 * - If SSE is near 0: The group moves as a perfect rigid bone.
 * - If SSE is high: The group is stretching or contains multiple independent motions.
 *
 * @return The total error (SSE) for the best possible rigid fit of this bone.
 */
double CalculateRigidSSE_WorldSpace(const std::vector<int>& indices,
                                    const std::vector<Eigen::Matrix3Xd>& allPoses,
                                    const Eigen::Matrix3Xd& restPose)
{
  if (indices.empty())
    return 0.0;
  const std::size_t N = indices.size();

  // 1. Reference Subset (3 x N)
  Eigen::Matrix3Xd P(3, N);
  for (std::size_t i = 0; i < N; ++i)
    P.col(i) = restPose.col(indices[i]);
  Eigen::Vector3d p_mean = P.rowwise().mean();
  Eigen::Matrix3Xd P_centered = P.colwise() - p_mean;

  double totalSSE = 0.0;

  for (const auto& pose : allPoses)
  {
    // 2. Target Subset (3 x N)
    Eigen::Matrix3Xd Q(3, N);
    for (std::size_t i = 0; i < N; ++i)
      Q.col(i) = pose.col(indices[i]);
    Eigen::Vector3d q_mean = Q.rowwise().mean();
    Eigen::Matrix3Xd Q_centered = Q.colwise() - q_mean;

    // 3. Kabsch Rotation
    Eigen::Matrix3d H = P_centered * Q_centered.transpose();  // Note: P*Q' for ref->pose
    Eigen::JacobiSVD<Eigen::Matrix3d> svd(H, Eigen::ComputeFullU | Eigen::ComputeFullV);

    Eigen::Matrix3d V = svd.matrixV();
    Eigen::Matrix3d U = svd.matrixU();
    Eigen::Matrix3d R = V * U.transpose();

    if (R.determinant() < 0)
    {
      V.col(2) *= -1;
      R = V * U.transpose();
    }

    // 4. Full Translation
    Eigen::Vector3d T = q_mean - R * p_mean;

    // 5. World Space Error (The Ultimate Truth)
    for (std::size_t i = 0; i < N; ++i)
    {
      Eigen::Vector3d reconstructed = R * P.col(i) + T;
      totalSSE += (reconstructed - Q.col(i)).squaredNorm();
    }
  }
  return totalSSE;
}

double CalculateSingleVertexSSE(int vIdx, const std::vector<Eigen::Matrix3Xd>& allPoses,
                                const Eigen::Matrix3Xd& restPose)
{
  double sse = 0.0;
  Eigen::Vector3d restPos = restPose.col(vIdx);

  for (const auto& pose : allPoses)
  {
    // We measure how far the vertex traveled from its rest position
    sse += (pose.col(vIdx) - restPos).squaredNorm();
  }
  return sse;
}

double FindNarrowestSplitAxis(const std::vector<int>& indices, const Eigen::Matrix3Xd& restPose)
{
  Eigen::Vector3d minB(1e9, 1e9, 1e9), maxB(-1e9, -1e9, -1e9);
  for (int idx : indices)
  {
    Eigen::Vector3d p = restPose.col(idx);
    minB = minB.cwiseMin(p);
    maxB = maxB.cwiseMax(p);
  }
  Eigen::Vector3d span = maxB - minB;
  if (span.x() < span.y() && span.x() < span.z())
    return 0;
  if (span.y() < span.x() && span.y() < span.z())
    return 1;
  return 2;
}

// --- Add frame importance calculation helper ---
double ComputeFrameImportance(const Eigen::Matrix3Xd& pose, const Eigen::Matrix3Xd& restPose)
{
  // Use variance of motion as importance
  double total = 0.0;
  for (int i = 0; i < pose.cols(); ++i)
    total += (pose.col(i) - restPose.col(i)).squaredNorm();
  return std::sqrt(total / pose.cols());
}

// --- Hybrid split function: motion + geometry ---
void rigid_split_raw_hybrid(const std::vector<Eigen::Matrix3Xd>& allPoses,
                            const std::vector<int>& targetIndices, std::vector<int>& assignments,
                            const Eigen::Matrix3Xd& restPose)
{
  // Weighted motion descriptor
  const std::size_t N_sub = targetIndices.size();
  const std::size_t K = allPoses.size();
  Eigen::MatrixXd motion(N_sub, 3 * K);
  std::vector<double> frame_weights(K);
  for (std::size_t k = 0; k < K; ++k)
    frame_weights[k] = ComputeFrameImportance(allPoses[k], restPose);

  for (std::size_t i = 0; i < N_sub; ++i)
  {
    int vIdx = targetIndices[i];
    for (std::size_t k = 0; k < K; ++k)
      motion.row(i).segment<3>(k * 3) = frame_weights[k] * allPoses[k].col(vIdx);
  }

  // Standard motion clustering
  int s1 = 0;
  int s2 = -1;
  double maxD = -1;
  for (std::size_t i = 1; i < N_sub; ++i)
  {
    double d = (motion.row(i) - motion.row(s1)).squaredNorm();
    if (d > maxD)
    {
      maxD = d;
      s2 = static_cast<int>(i);
    }
  }
  Eigen::RowVectorXd c1 = motion.row(s1);
  Eigen::RowVectorXd c2 = motion.row(s2);

  for (int iter = 0; iter < 15; ++iter)
  {
    std::vector<int> g1_local, g2_local;
    for (std::size_t i = 0; i < N_sub; ++i)
    {
      double d1 = (motion.row(i) - c1).squaredNorm();
      double d2 = (motion.row(i) - c2).squaredNorm();
      int best_k = (d1 < d2) ? 0 : 1;
      assignments[targetIndices[i]] = best_k;
      if (best_k == 0)
        g1_local.push_back(i);
      else
        g2_local.push_back(i);
    }
    if (g1_local.empty() || g2_local.empty())
      break;
    c1.setZero();
    for (int idx : g1_local)
      c1 += motion.row(idx);
    c1 /= static_cast<double>(g1_local.size());
    c2.setZero();
    for (int idx : g2_local)
      c2 += motion.row(idx);
    c2 /= static_cast<double>(g2_local.size());
  }

  // Geometric bottleneck override
  int axis = FindNarrowestSplitAxis(targetIndices, restPose);
  double minA = 1e9, maxA = -1e9;
  for (int idx : targetIndices)
  {
    double val = restPose(axis, idx);
    minA = std::min(minA, val);
    maxA = std::max(maxA, val);
  }
  double midA = (minA + maxA) / 2.0;
  // If group is very narrow, force split at midpoint
  if ((maxA - minA) < 0.15)  // threshold: tweak as needed
  {
    for (int idx : targetIndices)
    {
      double val = restPose(axis, idx);
      assignments[idx] = (val < midA) ? 0 : 1;
    }
  }
}

/**
 * Orchestrates the recursive bone discovery process using an SSE-based "Gate".
 *
 * This is the "Brain" of the bone detection. It identifies which vertex groups
 * are the most 'rubbery' (high World-Space SSE) and uses 'rigid_split_raw' to
 * break them down. It includes safeguard 'Gates' to prevent the creation of
 * microscopic bones or bones that don't significantly improve the animation's accuracy.
 *
 * @param tolerance The SSE threshold that triggers a split.
 * @return A flat array of Bone IDs for every vertex in the mesh.
 */
std::vector<int> CalculateVertexGroupsAdaptive(const std::vector<Eigen::Matrix3Xd>& allPoses,
                                               const Eigen::Matrix3Xd& restPose, double tolerance)
{
  const int numVertices = restPose.cols();
  std::vector<int> allIndices(numVertices);
  std::iota(allIndices.begin(), allIndices.end(), 0);

  struct BoneGroup
  {
    std::vector<int> indices;
    bool skip_split = false;
  };

  std::vector<BoneGroup> groups = {{allIndices, false}};
  const int MAX_BONES = 50;

  const double totalInitialSSE = CalculateRigidSSE_WorldSpace(allIndices, allPoses, restPose);

  while (groups.size() < MAX_BONES)
  {
    int split_index = -1;
    double maxSSE = -1;

    // Find worst group that isn't already "done"
    for (std::size_t i = 0; i < groups.size(); ++i)
    {
      if (groups[i].skip_split)
        continue;
      double sse = CalculateRigidSSE_WorldSpace(groups[i].indices, allPoses, restPose);
      if (sse > tolerance && sse > maxSSE)
      {
        maxSSE = sse;
        split_index = static_cast<int>(i);
      }
    }

    if (split_index == -1)
      break;

    // Perform the RAW motion split
    std::vector<int> sub_assignments(numVertices, -1);
    rigid_split_raw_hybrid(allPoses, groups[split_index].indices, sub_assignments, restPose);

    std::vector<int> g1, g2;
    for (int vIdx : groups[split_index].indices)
    {
      if (sub_assignments[vIdx] == 0)
        g1.push_back(vIdx);
      else
        g2.push_back(vIdx);
    }

    if (g1.empty() || g2.empty())
    {
      groups[split_index].skip_split = true;
      continue;
    }

    // IMPROVEMENT GATE (Using SSE for mathematical fairness)
    // Calculate total error
    const double parentSSE =
        CalculateRigidSSE_WorldSpace(groups[split_index].indices, allPoses, restPose);
    const double child1SSE = CalculateRigidSSE_WorldSpace(g1, allPoses, restPose);
    const double child2SSE = CalculateRigidSSE_WorldSpace(g2, allPoses, restPose);
    const double combinedChildSSE = child1SSE + child2SSE;

    // double improvementFactor = combinedChildSSE / parentSSE;

    /*WARN_LOG_FMT(
        VIDEO,
        "Step: {} | Parent size: {} | Child 1 size: {} | Child 2 size: {}, Improvement Factor: {}, "
        "Active "
        "groups: {}, Parent SSE: {} | Child 1 SSE: {} | Child 2 SSE: {} | Combined SSE: {}",
        groups.size(), groups[split_index].indices.size(), g1.size(), g2.size(), improvementFactor,
        std::count_if(groups.begin(), groups.end(), [](auto& g) { return !g.skip_split; }),
        parentSSE, child1SSE, child2SSE, combinedChildSSE);*/

    const double improvement = parentSSE - combinedChildSSE;

    // GATE A: The "Significant Contribution" Check
    // If the improvement is less than 0.01% of the total mesh error, it's not worth a new bone.
    if (improvement < (totalInitialSSE * 0.0001))
    {
      /*WARN_LOG_FMT(
          VIDEO,
                   "Skipping parent group due to no significant contribution, actual improvement "
                   "{}, totalInitialSSE {}, minimum improvement required {}",
                   improvement, totalInitialSSE, totalInitialSSE * 0.001);*/
      groups[split_index].skip_split = true;
      continue;
    }

    // GATE B: The "Absolute Size" Check
    // Don't create bones smaller than 1% of the total mesh (e.g., 25 vertices for a 2500 mesh)
    if (g1.size() < (numVertices * 0.01) || g2.size() < (numVertices * 0.01))
    {
      /*WARN_LOG_FMT(VIDEO, "Skipping parent group due to small size, max vertices allowed {}",
                   numVertices * 0.01);*/
      groups[split_index].skip_split = true;
      continue;
    }

    // Commit the split
    groups.erase(groups.begin() + split_index);
    groups.emplace_back(g1, false);
    groups.emplace_back(g2, false);
  }

  // Map back to final array
  std::vector<int> finalAssignments(numVertices, 0);
  for (std::size_t g = 0; g < groups.size(); ++g)
  {
    for (int vIdx : groups[g].indices)
      finalAssignments[vIdx] = static_cast<int>(g);
  }
  return finalAssignments;
}

/**
 * Measures how much the motion varies within a single bucket.
 * High variance indicates the bucket contains multiple distinct animations.
 */
double CalculateTemporalVariance(const std::vector<int>& frame_ids,
                                 const std::vector<Eigen::Matrix3Xd>& allPoses)
{
  if (frame_ids.empty())
    return 0.0;

  const int numVerts = allPoses[0].cols();
  const int signatureDim = numVerts * 3;

  // 1. Compute the Mean Signature for this bucket
  Eigen::VectorXd mean_sig = Eigen::VectorXd::Zero(signatureDim);
  for (int f_id : frame_ids)
  {
    // We use the same 'Velocity' logic here for consistency
    int prev_f = std::max(0, f_id - 1);
    Eigen::Matrix3Xd delta = allPoses[f_id] - allPoses[prev_f];
    mean_sig += Eigen::Map<const Eigen::VectorXd>(delta.data(), signatureDim);
  }
  mean_sig /= static_cast<double>(frame_ids.size());

  // 2. Calculate average squared distance from the mean
  double total_variance = 0.0;
  for (int f_id : frame_ids)
  {
    int prev_f = std::max(0, f_id - 1);
    Eigen::Matrix3Xd delta = allPoses[f_id] - allPoses[prev_f];
    Eigen::VectorXd sig = Eigen::Map<const Eigen::VectorXd>(delta.data(), signatureDim);
    total_variance += (sig - mean_sig).squaredNorm();
  }

  return total_variance / static_cast<double>(frame_ids.size());
}

/**
 * Calculates the Total Sum of Squared Error for a group of frames.
 * This represents the "Motion Volume" of a bucket.
 */
double CalculateTemporalSSE(const std::vector<int>& frame_ids,
                            const std::vector<Eigen::Matrix3Xd>& allPoses)
{
  if (frame_ids.empty())
    return 0.0;

  const int numVerts = allPoses[0].cols();
  const int signatureDim = numVerts * 3;

  // 1. Compute the Mean Signature for this group of frames
  Eigen::VectorXd mean_sig = Eigen::VectorXd::Zero(signatureDim);
  for (int f_id : frame_ids)
  {
    // Use the same 'Velocity' logic for consistency
    const int STRIDE = 5;  // Look 5 frames back (approx 1/12th of a second at 60fps)
    int prev_f = std::max(0, f_id - STRIDE);
    Eigen::Matrix3Xd delta = allPoses[f_id] - allPoses[prev_f];
    mean_sig += Eigen::Map<const Eigen::VectorXd>(delta.data(), signatureDim);
  }
  mean_sig /= static_cast<double>(frame_ids.size());

  // 2. Sum of Squared Distances from that Mean
  double total_sse = 0.0;
  for (int f_id : frame_ids)
  {
    const int STRIDE = 5;  // Look 5 frames back (approx 1/12th of a second at 60fps)
    int prev_f = std::max(0, f_id - STRIDE);
    Eigen::Matrix3Xd delta = allPoses[f_id] - allPoses[prev_f];
    Eigen::VectorXd sig = Eigen::Map<const Eigen::VectorXd>(delta.data(), signatureDim);

    // This is the SSE: (Point - Mean)^2
    total_sse += (sig - mean_sig).squaredNorm();
  }

  return total_sse;
}

/**
 * Slices a group of frames into two distinct sub-groups.
 * Uses a binary split strategy similar to your 'rigid_split_raw'.
 */
std::pair<std::vector<int>, std::vector<int>>
BinaryFrameSplit(const std::vector<int>& frame_ids, const std::vector<Eigen::Matrix3Xd>& allPoses,
                 const Eigen::Matrix3Xd& restPose)
{
  const int numFrames = frame_ids.size();
  const int signatureDim = restPose.cols() * 3;

  // 1. Extract signatures for all frames in this bucket
  std::vector<Eigen::VectorXd> sigs;
  const int STRIDE = 10;
  for (int f_id : frame_ids)
  {
    int prev_f = std::max(0, f_id - STRIDE);
    Eigen::Matrix3Xd delta = allPoses[f_id] - allPoses[prev_f];
    sigs.push_back(Eigen::Map<const Eigen::VectorXd>(delta.data(), signatureDim));
  }

  // Instead of the 'Max Distance' (which finds glitches),
  // we pick the frames at 1/4 and 3/4 of the way through the group.
  // This ensures we pick two points that are far apart in TIME.
  int s1 = numFrames / 4;
  int s2 = (3 * numFrames) / 4;

  Eigen::VectorXd c1 = sigs[s1];
  Eigen::VectorXd c2 = sigs[s2];

  // 3. Binary K-Means refinement (5-10 iterations is plenty for 1D timeline splitting)
  std::vector<int> g1_indices, g2_indices;
  for (int iter = 0; iter < 10; ++iter)
  {
    g1_indices.clear();
    g2_indices.clear();
    for (int i = 0; i < numFrames; ++i)
    {
      double d1 = (sigs[i] - c1).squaredNorm();
      double d2 = (sigs[i] - c2).squaredNorm();
      if (d1 < d2)
        g1_indices.push_back(i);
      else
        g2_indices.push_back(i);
    }

    if (g1_indices.empty() || g2_indices.empty())
      break;

    // Update centers
    c1.setZero();
    for (int idx : g1_indices)
      c1 += sigs[idx];
    c1 /= g1_indices.size();
    c2.setZero();
    for (int idx : g2_indices)
      c2 += sigs[idx];
    c2 /= g2_indices.size();
  }

  // Convert local indices back to global frame IDs
  std::vector<int> g1, g2;
  for (int idx : g1_indices)
    g1.push_back(frame_ids[idx]);
  for (int idx : g2_indices)
    g2.push_back(frame_ids[idx]);

  return {g1, g2};
}

/**
 * Segments the animation timeline into distinct "Temporal Buckets" based on
 * variations in character pose.
 *
 * Logic:
 * 1. Analyzes the total "Temporal SSE" (the deviation from a static pose).
 * 2. Uses a Binary K-Means approach to split the timeline where the motion
 *    is most complex (e.g., separating a "Walk" from a "Jump").
 * 3. Enforces "Statistical Stability" (Gate B) by ensuring buckets have enough
 *    frames to propose reliable, non-coincidental bones.
 * 4. Stops when the motion within each bucket is "Rigid Enough" (Tolerance)
 *    or the 64-bucket complexity limit is reached.
 *
 * Result: A set of frame-groups where the character's movement is internally
 * consistent, providing the best starting point for bone discovery.
 */
std::map<int, std::vector<int>>
AdaptiveTimelineSplitter(const std::vector<Eigen::Matrix3Xd>& allPoses,
                         const Eigen::Matrix3Xd& restPose,
                         double temporal_tolerance = 0.01)  // Adjust based on your capture noise
{
  const int numFrames = allPoses.size();
  std::vector<int> allIndices(numFrames);
  std::iota(allIndices.begin(), allIndices.end(), 0);

  struct BucketGroup
  {
    std::vector<int> frames;
    bool skip_split = false;
  };

  std::vector<BucketGroup> groups = {{allIndices, false}};

  // Calculate the 'Initial Temporal Error' (Total movement of the animation)
  const double totalInitialSSE = CalculateTemporalSSE(allIndices, allPoses);

  while (groups.size() < 64)
  {
    int split_idx = -1;
    double maxSSE = -1;

    // 1. SEARCH: Find the group that exceeds our NOISE FLOOR and is the "messiest"
    for (int i = 0; i < groups.size(); ++i)
    {
      if (groups[i].skip_split)
        continue;

      double sse = CalculateTemporalSSE(groups[i].frames, allPoses);

      // LESSON: Use the tolerance to ignore "Static" noise
      if (sse > temporal_tolerance && sse > maxSSE)
      {
        maxSSE = sse;
        split_idx = i;
      }
    }

    if (split_idx == -1)
      break;

    // 2. SPLIT: Binary K-Means on the frames
    auto [g1, g2] = BinaryFrameSplit(groups[split_idx].frames, allPoses, restPose);

    if (g1.empty() || g2.empty())
    {
      groups[split_idx].skip_split = true;
      continue;
    }

    // 3. GATE: The Improvement Check (Mirroring CalculateVertexGroupsAdaptive)
    double parentSSE = CalculateTemporalSSE(groups[split_idx].frames, allPoses);
    double combinedChildSSE =
        CalculateTemporalSSE(g1, allPoses) + CalculateTemporalSSE(g2, allPoses);

    WARN_LOG_FMT(VIDEO,
                 "Step: {} | Parent size: {} | Child 1 size: {} | Child 2 size: {}, "
                 "Active "
                 "groups: {}, Parent SSE: {} | Combined SSE: {}",
                 groups.size(), groups[split_idx].frames.size(), g1.size(), g2.size(),
                 std::count_if(groups.begin(), groups.end(), [](auto& g) { return !g.skip_split; }),
                 parentSSE, combinedChildSSE);

    double improvement = parentSSE - combinedChildSSE;

    // GATE A: Significant Contribution Check (0.1% of total motion)
    if (improvement < (totalInitialSSE * 0.001))
    {
      WARN_LOG_FMT(VIDEO,
                   "Skipping parent group due to no significant contribution, actual improvement "
                   "{}, totalInitialSSE {}, minimum improvement required {}",
                   improvement, totalInitialSSE, totalInitialSSE * 0.001);
      groups[split_idx].skip_split = true;
      continue;
    }

    // GATE B: Physical Size Check (No bucket < max frames)
    const std::size_t max_frames = 8;
    if (g1.size() < max_frames || g2.size() < max_frames)
    {
      WARN_LOG_FMT(VIDEO, "Skipping parent group due to small size, max frames allowed {}",
                   max_frames);
      groups[split_idx].skip_split = true;
      continue;
    }

    // COMMIT THE SPLIT
    groups.erase(groups.begin() + split_idx);
    groups.push_back({g1, false});
    groups.push_back({g2, false});
  }

  // [Convert back to map...]
  std::map<int, std::vector<int>> result;
  for (int i = 0; i < groups.size(); ++i)
    result[i] = groups[i].frames;
  return result;
}

void PrintWeightConflictDiagnostic(const Eigen::MatrixXd& W,
                                   const SimpleGeodesic::HeatGeodesicsData& heat_data)
{
  INFO_LOG_FMT(VIDEO, "--- WEIGHT CONFLICT: NECK REGION (Geo 0.25 - 0.35) ---");
  INFO_LOG_FMT(VIDEO, "{:<8} {:<10} {:<10} {:<10}", "VertID", "GeoDist", "Top Bone", "Top Weight");

  int count = 0;
  for (int i = 0; i < W.rows(); ++i)
  {
    double g = heat_data.geo_from_center(i);
    if (g > 0.25 && g < 0.35)
    {  // The "Transit Zone" between Head and Chest
      int maxBone = 0;
      double maxW = W.row(i).maxCoeff(&maxBone);
      if (count++ < 20)
      {
        INFO_LOG_FMT(VIDEO, "{:<8} {:<10.3f} {:<10} {:<10.3f}", i, g, maxBone, maxW);
      }
    }
  }
}

int IdentifyAnatomicalRoot_V2(const std::vector<int>& assignments,
                              const SimpleGeodesic::HeatGeodesicsData& heat_data)
{
  int numBones = 0;
  for (int b : assignments)
    numBones = std::max(numBones, b + 1);

  int bestRoot = -1;
  double minGeo = 1e12;

  for (int i = 0; i < assignments.size(); ++i)
  {
    // We look for the vertex that is literally the closest to the
    // geodesic source point (the pelvis match we lit).
    if (heat_data.geo_from_center(i) < minGeo)
    {
      minGeo = heat_data.geo_from_center(i);
      bestRoot = assignments[i];
    }
  }
  return bestRoot;
}

/**
 * @brief Converts 'Hard' bone assignments into 'Smooth' Linear Blend Skinning (LBS) weights.
 *
 * Uses the Heat Equation with a "Sovereign Shield" to protect small anatomical
 * bones (like the Neck or Hands) from being drowned out by the massive Spine/Core.
 */
Eigen::MatrixXd ComputeHarmonicWeights(const std::vector<int>& assignments,
                                       const Eigen::Matrix3Xd& V, const Eigen::MatrixXi& F,
                                       const std::vector<Eigen::Matrix3Xd>& allPoses,
                                       const SimpleGeodesic::HeatGeodesicsData& heat_data,
                                       int numBones)
{
  const int N = static_cast<int>(V.cols());
  Eigen::MatrixXd W = Eigen::MatrixXd::Zero(N, numBones);

  // 1. Identify the Anatomical Anchor (Lowest GeoAvg)
  int anchorID = IdentifyAnatomicalRoot_V2(assignments, heat_data);

  // 2. Pre-calculate Bone Metadata for the Veto
  struct BoneMeta
  {
    double minGeo;
    double avgGeo;
    bool exists;
  };
  std::vector<BoneMeta> boneMeta(numBones, {1e12, 0.0, false});
  std::vector<int> counts(numBones, 0);

  for (int i = 0; i < N; ++i)
  {
    int b = assignments[i];
    boneMeta[b].minGeo = std::min(boneMeta[b].minGeo, heat_data.geo_from_center(i));
    boneMeta[b].avgGeo += heat_data.geo_from_center(i);
    counts[b]++;
    boneMeta[b].exists = true;
  }
  for (int b = 0; b < numBones; ++b)
  {
    if (counts[b] > 0)
      boneMeta[b].avgGeo /= counts[b];
  }

  // 3. Solve Heat with Veto and Masking
  for (int b = 0; b < numBones; ++b)
  {
    if (!boneMeta[b].exists)
      continue;

    Eigen::VectorXd delta = Eigen::VectorXd::Zero(N);
    for (int i = 0; i < N; ++i)
      if (assignments[i] == b)
        delta(i) = 1.0;

    Eigen::VectorXd proximity = heat_data.heat_solver.solve(delta);
    double maxp = proximity.maxCoeff();
    if (maxp > 1e-12)
      proximity /= maxp;

    // --- THE SCIENTIFIC FILTERS ---
    for (int i = 0; i < N; ++i)
    {
      double vGeo = heat_data.geo_from_center(i);

      // A. UPSTREAM VETO: A bone cannot influence vertices
      // closer to the heart (Anchor) than its own starting point.
      if (b != anchorID && vGeo < boneMeta[b].minGeo)
      {
        proximity(i) = 0.0;
      }

      // B. GEODESIC MASK: Exponential falloff beyond a "Joint Radius"
      double distToBone = std::abs(vGeo - boneMeta[b].avgGeo);
      if (distToBone > 0.15)
      {
        proximity(i) *= std::exp(-10.0 * (distToBone - 0.15));
      }
    }

    // C. SHARPEN: Power of 4 for tighter transitions
    for (int i = 0; i < N; ++i)
    {
      W(i, b) = std::pow(std::max(0.0, proximity(i)), 4.0);
    }
  }

  // 4. WINNER-TAKE-MOST: Force commitment in conflict zones
  for (int i = 0; i < N; ++i)
  {
    int b1 = -1;
    double w1 = -1.0, w2 = -1.0;
    for (int b = 0; b < numBones; ++b)
    {
      if (W(i, b) > w1)
      {
        w2 = w1;
        w1 = W(i, b);
        b1 = b;
      }
      else if (W(i, b) > w2)
      {
        w2 = W(i, b);
      }
    }
    // If winner is dominant, kill the "chatter" from other bones
    if (w1 > w2 * 2.0)
    {
      for (int b = 0; b < numBones; ++b)
      {
        if (b != b1 && W(i, b) < 0.1)
          W(i, b) = 0.0;
      }
    }
  }

  // 5. Final Normalization
  for (int i = 0; i < N; ++i)
  {
    double s = W.row(i).sum();
    if (s > 1e-9)
      W.row(i) /= s;
    else
      W(i, assignments[i]) = 1.0;
  }

  PrintWeightConflictDiagnostic(W, heat_data);
  return W;
}

/**
 * @brief Computes a centroid weighted by "Rigidity".
 *
 * High-stretch vertices (neck, armpit) are given low weight.
 * Rigid vertices (skull, mid-limb) are given high weight.
 * Result: The pivot naturally moves toward the "bone" and away from the "skin".
 */
Eigen::Vector3d ComputeRobustCentroid(const std::vector<int>& indices,
                                      const std::vector<Eigen::Matrix3Xd>& allPoses,
                                      const Eigen::Matrix3Xd& restPose)
{
  if (indices.empty())
    return Eigen::Vector3d::Zero();

  const size_t N = indices.size();

  // 1. Manually build the Rest Pose matrix for this group
  Eigen::Matrix3Xd P(3, N);
  for (size_t i = 0; i < N; ++i)
  {
    P.col(i) = restPose.col(indices[i]);
  }

  const Eigen::Vector3d p_mean = P.rowwise().mean();
  const Eigen::Matrix3Xd P_centered = P.colwise() - p_mean;

  std::vector<double> vertexErrors(N, 0.0);
  double totalMaxError = 0.0;

  // 2. Analyze motion across all frames
  for (const auto& pose : allPoses)
  {
    // Build Target Pose matrix for this frame
    Eigen::Matrix3Xd Q(3, N);
    for (size_t i = 0; i < N; ++i)
    {
      Q.col(i) = pose.col(indices[i]);
    }

    const Eigen::Vector3d q_mean = Q.rowwise().mean();
    const Eigen::Matrix3Xd Q_centered = Q.colwise() - q_mean;

    // Solve Rotation R using SVD
    Eigen::Matrix3d H = Q_centered * P_centered.transpose();
    Eigen::JacobiSVD<Eigen::Matrix3d> svd(H, Eigen::ComputeFullU | Eigen::ComputeFullV);

    Eigen::Matrix3d U = svd.matrixU();
    Eigen::Matrix3d V = svd.matrixV();
    Eigen::Matrix3d R = U * V.transpose();

    if (R.determinant() < 0)
    {
      V.col(2) *= -1.0;
      R = U * V.transpose();
    }
    const Eigen::Vector3d T = q_mean - R * p_mean;

    // Accumulate Error: How much does each vertex "fight" the rigid motion?
    for (size_t i = 0; i < N; ++i)
    {
      Eigen::Vector3d expected = R * P.col(i) + T;
      double err = (expected - Q.col(i)).squaredNorm();
      vertexErrors[i] += err;
      totalMaxError = std::max(totalMaxError, vertexErrors[i]);
    }
  }

  // 3. Final Weighted Centroid
  Eigen::Vector3d weightedCentroid = Eigen::Vector3d::Zero();
  double totalWeight = 0.0;

  for (size_t i = 0; i < N; ++i)
  {
    // Rigid vertices (low error) get weight 1.0.
    // Stretchy vertices (high error) get weight near 0.0.
    // sigma prevents division by zero and controls the "falloff"
    double sigma = (totalMaxError / 10.0) + 1e-7;
    double w = 1.0 / (1.0 + (vertexErrors[i] / sigma));

    weightedCentroid += P.col(i) * w;
    totalWeight += w;
  }

  return weightedCentroid / totalWeight;
}

/**
 * Calculates the 3D 'Pivot Point' for every discovered bone.
 *
 * For each bone group, it finds the geometric center (centroid) of its
 * assigned vertices in the rest pose. These points act as the 'Joints'
 * for the glTF skeleton and the origin for all rotations.
 */
std::vector<Eigen::Vector3d> ComputeBoneRestPositions(const std::vector<std::vector<int>>& groups,
                                                      const std::vector<Eigen::Matrix3Xd>& allPoses,
                                                      const Eigen::Matrix3Xd& restPose)
{
  std::vector<Eigen::Vector3d> bonePositions;
  for (const auto& groupIndices : groups)
  {
    // Use the new solver here
    bonePositions.push_back(ComputeRobustCentroid(groupIndices, allPoses, restPose));
  }
  return bonePositions;
}

/**
 * Removes 'parasitic' weights by enforcing physical connectivity.
 *
 * Sometimes two distant parts (like a hand and a foot) move in sync, causing
 * the math to group them together. This function finds 'islands' of vertices
 * that aren't physically touching the main body of a bone and 'snaps' them
 * back to their actual physical neighbors.
 */
void CleanBoneIslands(std::vector<int>& assignments, const Eigen::MatrixXi& F, int numVertices,
                      int numBones, int max_gpu_skinned_id)
{
  // 1. Build Adjacency
  std::vector<std::vector<int>> adj(numVertices);
  for (int i = 0; i < F.rows(); ++i)
  {
    for (int j = 0; j < 3; ++j)
    {
      int u = F(i, j);
      int v = F(i, (j + 1) % 3);
      adj[u].push_back(v);
      adj[v].push_back(u);
    }
  }

  // 2. Group into islands
  std::vector<int> islandID(numVertices, -1);
  std::vector<std::vector<int>> islands;
  std::vector<int> islandBoneType;

  std::vector<bool> visited(numVertices, false);
  for (int i = 0; i < numVertices; ++i)
  {
    if (visited[i])
      continue;

    const int current_bone = assignments[i];

    // Don't modify gpu skinned bones
    if (current_bone <= max_gpu_skinned_id)
      continue;

    std::vector<int> component;
    std::queue<int> q;
    q.push(i);
    visited[i] = true;

    while (!q.empty())
    {
      int u = q.front();
      q.pop();
      component.push_back(u);
      islandID[u] = (int)islands.size();
      for (int v : adj[u])
      {
        const int neighbor_bone = assignments[v];
        if (!visited[v] && neighbor_bone == current_bone)
        {
          visited[v] = true;
          q.push(v);
        }
      }
    }
    islandBoneType.push_back(current_bone);
    islands.push_back(component);
  }

  // 3. Find the "Alpha" island for each bone (the largest one)
  std::vector<int> alphaIslandForBone(numBones, -1);
  std::vector<size_t> maxSizes(numBones, 0);
  for (std::size_t i = 0; i < islands.size(); i++)
  {
    int b = islandBoneType[i];
    if (islands[i].size() > maxSizes[b])
    {
      maxSizes[b] = islands[i].size();
      alphaIslandForBone[b] = i;
    }
  }

  // 4. Snap Orphans: If you're not in your bone's alpha island,
  // take the bone ID of a neighboring vertex that IS in an alpha island.
  for (int i = 0; i < numVertices; i++)
  {
    int myIsland = islandID[i];
    int myBone = assignments[i];

    if (myIsland != alphaIslandForBone[myBone])
    {
      // I'm an orphan! Find a neighbor to adopt me.
      for (int neighbor : adj[i])
      {
        int nIsland = islandID[neighbor];
        int nBone = assignments[neighbor];
        // Only adopt if the neighbor is part of a "legit" main bone body
        if (nIsland == alphaIslandForBone[nBone])
        {
          assignments[i] = nBone;
          break;
        }
      }
    }
  }
}

std::vector<int> FinalMergeProposals_Deterministic(int numVertices,
                                                   const std::vector<Eigen::Matrix3Xd>& allPoses,
                                                   const Eigen::Matrix3Xd& restPose,
                                                   const Eigen::MatrixXi& F)
{
  const int numFrames = static_cast<int>(allPoses.size());

  // 1. Pre-calculate Motion Signatures (Displacement Trajectories)
  // We normalize each signature to make the Dot Product equal to Correlation.
  Eigen::MatrixXd signatures(numVertices, numFrames * 3);
  for (int i = 0; i < numVertices; ++i)
  {
    Eigen::VectorXd traj(numFrames * 3);
    for (int f = 0; f < numFrames; ++f)
    {
      traj.segment<3>(f * 3) = allPoses[f].col(i) - restPose.col(i);
    }

    // Add a tiny epsilon to the norm to prevent division by zero for static verts
    double n = traj.norm();
    if (n > 1e-9)
      traj /= n;
    signatures.row(i) = traj;
  }

  // 2. Disjoint Set Union (DSU) to merge neighbors with high correlation
  std::vector<int> parent(numVertices);
  std::iota(parent.begin(), parent.end(), 0);
  auto find_root = [&](int i) {
    int r = i;
    while (parent[r] != r)
      r = parent[r];
    return r;
  };

  // Correlation Threshold: 0.999 is very strict (essentially same motion)
  // Adjust to 0.995 if you still see tiny fragments in the face.
  const double threshold = 0.999;

  // Iterate through every edge in the mesh
  for (int i = 0; i < F.rows(); ++i)
  {
    for (int j = 0; j < 3; ++j)
    {
      int u = F(i, j);
      int v = F(i, (j + 1) % 3);

      int root_u = find_root(u);
      int root_v = find_root(v);
      if (root_u == root_v)
        continue;

      // Dot product of normalized signatures = Pearson Correlation
      double correlation = signatures.row(u).dot(signatures.row(v));

      if (correlation > threshold)
      {
        parent[root_u] = root_v;
      }
    }
  }

  // 3. Compact IDs and Return
  std::vector<int> final_assignments(numVertices);
  std::map<int, int> root_to_id;
  int next_id = 0;
  for (int i = 0; i < numVertices; ++i)
  {
    int r = find_root(i);
    if (root_to_id.find(r) == root_to_id.end())
    {
      root_to_id[r] = next_id++;
    }
    final_assignments[i] = root_to_id[r];
  }

  INFO_LOG_FMT(VIDEO, "Merging complete. Discovered {} total bones across all animation buckets.",
               next_id);

  return final_assignments;
}

int CountSharedEdges(int b1, int b2, const std::vector<int>& assignments, const Eigen::MatrixXi& F)
{
  int shared = 0;
  for (int i = 0; i < F.rows(); ++i)
  {
    int u = assignments[F(i, 0)], v = assignments[F(i, 1)], w = assignments[F(i, 2)];
    if ((u == b1 && v == b2) || (u == b2 && v == b1))
      shared++;
    if ((v == b1 && w == b2) || (v == b2 && w == b1))
      shared++;
    if ((w == b1 && u == b2) || (w == b2 && u == b1))
      shared++;
  }
  return shared;
}

std::vector<Eigen::Matrix3Xd> GetPosesForFrames(const std::vector<Eigen::Matrix3Xd>& allPoses,
                                                const std::vector<int>& frames)
{
  std::vector<Eigen::Matrix3Xd> subset;
  for (int f : frames)
  {
    subset.push_back(allPoses[f]);
  }
  return subset;
}

/**
 * Calculates the "Worst Vertex Displacement" if two bone groups are unified.
 *
 * Logic:
 * 1. Merges the two vertex sets.
 * 2. For every frame of animation:
 *    a. Centers the vertices to remove translation.
 *    b. Uses SVD (Kabsch) to find the optimal rotation to align the
 *       Rest Pose to the Current Pose.
 *    c. Applies that rotation/translation back to the Rest Pose.
 *    d. Measures the distance (error) for every vertex.
 * 3. Returns the highest single-vertex error found across ALL frames.
 *
 * @return The Max Distance Error (MDE) in world units.
 */
double CalculateMaxMergeError(const std::vector<int>& indices1, const std::vector<int>& indices2,
                              const std::vector<Eigen::Matrix3Xd>& allPoses,
                              const Eigen::Matrix3Xd& restPose)
{
  std::vector<int> combined = indices1;
  combined.insert(combined.end(), indices2.begin(), indices2.end());
  if (combined.empty())
    return 0.0;

  const int N = (int)combined.size();
  Eigen::Matrix3Xd P(3, N);
  for (int i = 0; i < N; ++i)
    P.col(i) = restPose.col(combined[i]);

  Eigen::Vector3d p_mean = P.rowwise().mean();
  Eigen::Matrix3Xd P_centered = P.colwise() - p_mean;

  double maxMDE = 0.0;

  for (const auto& pose : allPoses)
  {
    Eigen::Matrix3Xd Q(3, N);
    for (int i = 0; i < N; ++i)
      Q.col(i) = pose.col(combined[i]);

    Eigen::Vector3d q_mean = Q.rowwise().mean();
    Eigen::Matrix3Xd Q_centered = Q.colwise() - q_mean;

    // Kabsch Algorithm for BEST rigid fit
    Eigen::Matrix3d H = P_centered * Q_centered.transpose();
    Eigen::JacobiSVD<Eigen::Matrix3d> svd(H, Eigen::ComputeFullU | Eigen::ComputeFullV);
    Eigen::Matrix3d R = svd.matrixV() * svd.matrixU().transpose();
    if (R.determinant() < 0)
    {
      Eigen::Matrix3d V = svd.matrixV();
      V.col(2) *= -1;
      R = V * svd.matrixU().transpose();
    }
    Eigen::Vector3d T = q_mean - R * p_mean;

    // Calculate the WORST vertex error in this frame
    for (int i = 0; i < N; ++i)
    {
      Eigen::Vector3d target = Q.col(i);
      Eigen::Vector3d solved = R * P.col(i) + T;
      double err = (target - solved).norm();  // Euclidean distance error
      if (err > maxMDE)
        maxMDE = err;
    }
  }
  return maxMDE;
}

struct MergeTask
{
  int u, v;
  double mde;
  std::vector<int> indicesU, indicesV, combined;
};

/**
 * Scans the mesh triangles to find which bones are physically touching.
 *
 * Logic: An edge in a triangle is a "border" if its two vertices belong to
 * different bones. This identifies all adjacent bone pairs.
 *
 * @param assignments The current bone ID for every vertex in the mesh.
 * @param F The triangle face matrix (indices of vertices for every face).
 * @return A set of unique pairs {ID_A, ID_B} where ID_A < ID_B.
 */
std::set<std::pair<int, int>> GetNeighborBonesSet(const std::vector<int>& assignments,
                                                  const Eigen::MatrixXi& F)
{
  std::set<std::pair<int, int>> pairs;
  for (int i = 0; i < F.rows(); ++i)
  {
    // A triangle is a "Meeting Room."
    // Any bones present in this triangle are neighbors.
    int b[3] = {assignments[F(i, 0)], assignments[F(i, 1)], assignments[F(i, 2)]};

    // Check every pair in the triangle (0-1, 1-2, 2-0)
    for (int j = 0; j < 3; ++j)
    {
      int next = (j + 1) % 3;
      if (b[j] != b[next] && b[j] != -1 && b[next] != -1)
      {
        pairs.insert({std::min(b[j], b[next]), std::max(b[j], b[next])});
      }
    }
  }
  return pairs;
}

void ApplyMergeToAssignments(std::vector<int>& assignments, int fromID, int toID)
{
  for (int& b : assignments)
  {
    if (b == fromID)
      b = toID;
  }
}

/**
 * Builds a list of all possible merge operations between neighboring bones.
 *
 * Logic:
 * 1. Groups all vertices by their current Bone ID.
 * 2. Finds all touching bone pairs via GetNeighborBonesSet.
 * 3. Calculates the "Worst Case" Error (MDE) if those two bones became one.
 */
std::vector<MergeTask> BuildMergeTasks(const std::vector<int>& assignments,
                                       const std::vector<Eigen::Matrix3Xd>& allPoses,
                                       const Eigen::Matrix3Xd& restPose, const Eigen::MatrixXi& F)
{
  int numBones = 0;
  for (int b : assignments)
  {
    numBones = std::max(numBones, b + 1);
  }

  std::vector<std::vector<int>> boneToIndices(numBones);
  for (int i = 0; i < assignments.size(); ++i)
  {
    if (assignments[i] != -1)
      boneToIndices[assignments[i]].push_back(i);
  }

  auto pairs = GetNeighborBonesSet(assignments, F);  // Triangle-based neighbor finder
  std::vector<MergeTask> tasks;
  for (auto const& p : pairs)
  {
    if (boneToIndices[p.first].empty() || boneToIndices[p.second].empty())
      continue;

    const double mde =
        CalculateMaxMergeError(boneToIndices[p.first], boneToIndices[p.second], allPoses, restPose);

    MergeTask task;
    task.u = p.first;
    task.v = p.second;
    task.mde = mde;
    task.indicesU = boneToIndices[p.first];
    task.indicesV = boneToIndices[p.second];
    task.combined = task.indicesU;
    task.combined.insert(task.combined.end(), task.indicesV.begin(), task.indicesV.end());

    tasks.push_back(task);
  }
  return tasks;
}

/**
 * @brief Solves for the optimal rigid body transformation (Rotation + Translation).
 *
 * Implements the Orthogonal Procrustes problem using SVD. Finds the 4x4 matrix
 * that minimizes the distance between the restPose and a set of target poses
 * for a specific subset of vertices.
 *
 * @return A vector of 4x4 matrices (one per frame in the bucket).
 */
std::vector<Eigen::Matrix4d> ComputeRigidTransforms(const std::vector<int>& indices,
                                                    const std::vector<Eigen::Matrix3Xd>& poses,
                                                    const Eigen::Matrix3Xd& restPose)
{
  std::vector<Eigen::Matrix4d> transforms;
  transforms.reserve(poses.size());

  // 1. Manually extract and center the rest points
  // (This replaces restPose(Eigen::all, indices))
  Eigen::Matrix3Xd source(3, indices.size());
  for (int i = 0; i < indices.size(); ++i)
  {
    source.col(i) = restPose.col(indices[i]);
  }

  Eigen::Vector3d centroid_s = source.rowwise().mean();
  Eigen::Matrix3Xd s_centered = source.colwise() - centroid_s;

  for (const auto& pose : poses)
  {
    // 2. Manually extract and center the animated points
    Eigen::Matrix3Xd target(3, indices.size());
    for (int i = 0; i < indices.size(); ++i)
    {
      target.col(i) = pose.col(indices[i]);
    }

    Eigen::Vector3d centroid_t = target.rowwise().mean();
    Eigen::Matrix3Xd t_centered = target.colwise() - centroid_t;

    // 3. SVD for optimal rotation
    Eigen::Matrix3d H = s_centered * t_centered.transpose();
    Eigen::JacobiSVD<Eigen::Matrix3d> svd(H, Eigen::ComputeFullU | Eigen::ComputeFullV);

    Eigen::Matrix3d R = svd.matrixV() * svd.matrixU().transpose();

    // 4. Calculate translation
    Eigen::Vector3d t = centroid_t - (R * centroid_s);

    // 5. Build the matrix
    Eigen::Matrix4d m4 = Eigen::Matrix4d::Identity();
    m4.block<3, 3>(0, 0) = R;
    m4.block<3, 1>(0, 3) = t;

    transforms.push_back(m4);
  }
  return transforms;
}

/**
 * @brief Measures the physical deviation of a vertex cluster from ideal rigidity.
 *
 * Calculates the Maximum Distance Error (MDE) in world units. It determines the
 * single worst vertex "drift" across all frames, providing a concrete metric
 * (e.g., 5mm) to audit potential bone merges.
 */
double CalculateMDE(const std::vector<int>& indices, const std::vector<Eigen::Matrix3Xd>& poses,
                    const Eigen::Matrix3Xd& restPose)
{
  double maxError = 0.0;

  // 1. Find the best rigid transform for this cluster across these specific poses
  // This is your Procrustes/SVD fit we used before
  std::vector<Eigen::Matrix4d> transforms = ComputeRigidTransforms(indices, poses, restPose);

  // 2. Check every vertex in every frame of this bucket
  for (size_t f = 0; f < poses.size(); ++f)
  {
    for (int vIdx : indices)
    {
      Eigen::Vector3d restP = restPose.col(vIdx);
      Eigen::Vector3d animatedP = poses[f].col(vIdx);

      // Transform the rest point by the calculated bone matrix
      Eigen::Vector3d predictedP = (transforms[f] * restP.homogeneous()).head<3>();

      // Find the Euclidean distance (the "drift")
      double dist = (animatedP - predictedP).norm();
      if (dist > maxError)
        maxError = dist;
    }
  }
  return maxError;
}

/**
 * @brief Merges vertex clusters only if they remain rigid across all motion types.
 *
 * Acts as a temporal judge. Instead of using abstract ratios, it performs a
 * "Bucket Veto": a merge is rejected if any single motion bucket (like a
 * facial twitch) causes the MDE to exceed the rigidityThreshold.
 */
std::vector<int> BucketConsensusMerger(
    std::vector<int> assignments, const std::vector<std::vector<Eigen::Matrix3Xd>>& bucket_data,
    const std::vector<Eigen::Matrix3Xd>& allPoses, const Eigen::Matrix3Xd& restPose,
    const Eigen::MatrixXi& F, int max_gpu_skinned_id, double rigidityThreshold = 0.005)
{
  bool changed = true;
  while (changed)
  {
    changed = false;

    // 1. Get the tasks using your EXISTING BuildMergeTasks
    auto tasks = BuildMergeTasks(assignments, allPoses, restPose, F);

    // 2. Sort so we handle the "Stoniest" (most rigid) merges first
    std::sort(tasks.begin(), tasks.end(), [](auto& a, auto& b) { return a.mde < b.mde; });

    std::set<int> mergedThisBatch;

    for (auto const& t : tasks)
    {
      if (mergedThisBatch.count(t.u) || mergedThisBatch.count(t.v))
        continue;

      bool u_is_gpu_skinned = (t.u <= max_gpu_skinned_id);
      bool v_is_gpu_skinned = (t.v <= max_gpu_skinned_id);

      // Ignore any that are gpu skinned, we know they are correct!
      if (u_is_gpu_skinned || v_is_gpu_skinned)
      {
        continue;
      }

      // --- THE BUCKET VETO ---
      // Instead of a "Ratio," we check the MDE in every motion bucket.
      bool isRigidInAllBuckets = true;
      for (const auto& poses_in_bucket : bucket_data)
      {
        // Use the MDE function we just defined
        double bucket_mde = CalculateMDE(t.combined, poses_in_bucket, restPose);

        if (bucket_mde > rigidityThreshold)
        {
          isRigidInAllBuckets = false;
          break;
        }
      }

      if (isRigidInAllBuckets)
      {
        ApplyMergeToAssignments(assignments, t.u, t.v);
        mergedThisBatch.insert(t.u);
        mergedThisBatch.insert(t.v);
        changed = true;
      }
    }
  }

  // Reduce the bone range
  std::map<int, int> oldToNew;
  int nextID = 0;

  // PASS 1: Lock in the Simulation IDs (0 to max_sim_id)
  // We map them to themselves to "reserve" their spots.
  for (int i = 0; i <= max_gpu_skinned_id; ++i)
  {
    oldToNew[i] = i;
  }
  nextID = max_gpu_skinned_id + 1;

  // PASS 2: Map the Discovered IDs to a contiguous range
  for (int& id : assignments)
  {
    if (id == -1)
      continue;

    // If it's a Discovered ID (> max_sim_id) and we haven't seen it yet...
    if (id > max_gpu_skinned_id && oldToNew.find(id) == oldToNew.end())
    {
      oldToNew[id] = nextID++;
    }

    id = oldToNew[id];
  }
  return assignments;
}

std::map<int, std::set<int>> GetNeighborBones(const std::vector<int>& assignments,
                                              const Eigen::MatrixXi& F)
{
  std::map<int, std::set<int>> neighbors;
  for (int i = 0; i < F.rows(); ++i)
  {
    int b1 = assignments[F(i, 0)];
    int b2 = assignments[F(i, 1)];
    int b3 = assignments[F(i, 2)];
    if (b1 != b2)
    {
      neighbors[b1].insert(b2);
      neighbors[b2].insert(b1);
    }
    if (b2 != b3)
    {
      neighbors[b2].insert(b3);
      neighbors[b3].insert(b2);
    }
    if (b3 != b1)
    {
      neighbors[b3].insert(b1);
      neighbors[b1].insert(b3);
    }
  }
  return neighbors;
}

void PrintMDEAnatomicalDiagnostic(const std::vector<int>& assignments,
                                  const std::vector<Eigen::Matrix3Xd>& allPoses,
                                  const Eigen::Matrix3Xd& restPose,
                                  const SimpleGeodesic::HeatGeodesicsData& heat_data)
{
  int numBones = 0;
  for (int b : assignments)
    numBones = std::max(numBones, b + 1);

  std::vector<std::vector<int>> boneToIndices(numBones);
  for (int i = 0; i < assignments.size(); ++i)
    boneToIndices[assignments[i]].push_back(i);

  int rootID = IdentifyAnatomicalRoot_V2(assignments, heat_data);

  // 2. Build ROOT-RELATIVE Trajectories
  // This is the "Mr. Fantastic" killer.
  std::vector<Eigen::VectorXd> relativeTrajs(numBones);
  std::vector<double> relSSE(numBones, 0.0);

  // Get the Root's global motion first
  Eigen::Matrix3Xd rootMotion = Eigen::Matrix3Xd::Zero(3, allPoses.size());
  for (size_t f = 0; f < allPoses.size(); ++f)
  {
    Eigen::Vector3d centroid = Eigen::Vector3d::Zero();
    for (int idx : boneToIndices[rootID])
      centroid += allPoses[f].col(idx);
    rootMotion.col(f) = centroid / (double)boneToIndices[rootID].size();
  }

  for (int b = 0; b < numBones; ++b)
  {
    if (boneToIndices[b].empty())
      continue;
    Eigen::VectorXd t = Eigen::VectorXd::Zero(allPoses.size() * 3);
    double totalSqDist = 0;

    for (size_t f = 0; f < allPoses.size(); ++f)
    {
      Eigen::Vector3d centroid = Eigen::Vector3d::Zero();
      for (int idx : boneToIndices[b])
      {
        Eigen::Vector3d pos = allPoses[f].col(idx);
        centroid += pos;

        // Displacement from rest position
        totalSqDist += (pos - restPose.col(idx)).squaredNorm();
      }
      centroid /= (double)boneToIndices[b].size();

      // SUBTRACT ROOT MOTION: Now we only see how the bone moves
      // relative to the hips!
      t.segment<3>(f * 3) = centroid - rootMotion.col(f);
    }
    double n = t.norm();
    if (n > 1e-9)
      t /= n;
    relativeTrajs[b] = t;

    // SSE per vertex per frame
    relSSE[b] = totalSqDist / (double)(boneToIndices[b].size() * allPoses.size());
  }

  auto neighbors = GetNeighborBones(assignments, heat_data.F);

  INFO_LOG_FMT(VIDEO, "--- MDE STABILITY DIAGNOSTIC ---");
  INFO_LOG_FMT(VIDEO, "{:<4} {:<6} {:<10} {:<10} {:<10} {:<10} {:<8} {:<12}", "ID", "Verts",
               "Rel.SSE", "Self-MDE", "Best-MDE", "Ratio", "GeoAvg", "Role");

  for (int b = 0; b < numBones; ++b)
  {
    if (boneToIndices[b].empty())
      continue;

    // 1. Internal Rigidity (How much this bone stretches on its own)
    double selfMDE = CalculateMaxMergeError(boneToIndices[b], {}, allPoses, restPose);
    double selfSSE = CalculateRigidSSE_WorldSpace(boneToIndices[b], allPoses, restPose);

    double bestMDE = 1.0;
    double bestRatio = 99.9;  // Default high

    for (int n : neighbors[b])
    {
      double mde = CalculateMaxMergeError(boneToIndices[b], boneToIndices[n], allPoses, restPose);
      if (mde < bestMDE)
      {
        bestMDE = mde;

        // Calculate the Merge Ratio for this specific neighbor
        double neighborSSE = CalculateRigidSSE_WorldSpace(boneToIndices[n], allPoses, restPose);
        std::vector<int> combined = boneToIndices[b];
        combined.insert(combined.end(), boneToIndices[n].begin(), boneToIndices[n].end());
        double combinedSSE = CalculateRigidSSE_WorldSpace(combined, allPoses, restPose);

        bestRatio = combinedSSE / (selfSSE + neighborSSE + 1e-9);
      }
    }

    double relCorr = relativeTrajs[b].dot(relativeTrajs[rootID]);

    double minG = 1e9, maxG = -1e9, sumG = 0;
    for (int idx : boneToIndices[b])
    {
      double g = heat_data.geo_from_center(idx);
      minG = std::min(minG, g);
      maxG = std::max(maxG, g);
      sumG += g;
    }
    double avgG = sumG / (double)boneToIndices[b].size();
    double spanG = maxG - minG;

    std::string role = (b == rootID)    ? "ANCHOR" :
                       (relCorr > 0.99) ? "STIFF_BODY" :
                       (spanG > 0.4)    ? "LONG_LIMB" :
                                          "JOINT";

    INFO_LOG_FMT(VIDEO, "{:<4} {:<6} {:<10.6f} {:<10.6f} {:<10.6f} {:<10.2f} {:<8.3f} {:<12}", b,
                 boneToIndices[b].size(), relSSE[b], selfMDE, bestMDE, bestRatio, avgG, role);
  }
}

/**
 * @brief Unifies GPU skinned IDs with Discovered Bone IDs.
 *
 * @param sim_ids The IDs provided per-vertex by the gpu skinning (-1 if none).
 * @param discovered_shards The high-resolution shards found by our algorithm.
 * @return A unified assignment vector where GPU skinned IDs are preserved and
 *         Discovered IDs are offset to avoid collisions.
 */
std::vector<int> ResolveHybridAssignments(const std::vector<int>& gpu_skinned_ids,
                                          const std::vector<int>& discovered_shards,
                                          const Eigen::VectorXi& SVJ, int* max_gpu_skinned_id)
{
  if (gpu_skinned_ids.empty())
    return discovered_shards;

  const int welded_assignment_count = discovered_shards.size();
  std::vector<int> final_assignments(welded_assignment_count, -1);

  // Find the highest ID used by the gpu skinned to set our "Safety Offset"
  *max_gpu_skinned_id = -1;
  for (int id : gpu_skinned_ids)
  {
    if (id > *max_gpu_skinned_id)
      *max_gpu_skinned_id = id;
  }

  // Any discovered ID will start after the gpu skinned range
  int id_offset = *max_gpu_skinned_id + 1;

  for (std::size_t i = 0; i < gpu_skinned_ids.size(); ++i)
  {
    const int new_index = SVJ[i];
    if (final_assignments[new_index] != -1)
    {
      INFO_LOG_FMT(VIDEO, "Index {} is not empty on the mesh!", new_index);
      continue;
    }

    if (gpu_skinned_ids[i] != -1)
    {
      // Priority 1: Respect the gpu skinned ID
      final_assignments[new_index] = gpu_skinned_ids[i];
    }
    else
    {
      // Priority 2: Use our discovered shard, offset to prevent collision
      final_assignments[new_index] = discovered_shards[new_index] + id_offset;
    }
  }

  return final_assignments;
}

/**
 * The high-level orchestrator for the new bucket-driven rigging system.
 */
std::vector<int> GenerateRigFromMotion(const std::vector<Eigen::Matrix3Xd>& allPoses,
                                       const Eigen::Matrix3Xd& restPose,
                                       const SimpleGeodesic::HeatGeodesicsData& heat_data,
                                       const std::vector<int>& gpu_skinned_ids,
                                       const Eigen::VectorXi& SVJ)
{
  // Discovers how many "distinct" animations are in the capture (Ninja flips, thinking, etc).
  auto frame_buckets = AdaptiveTimelineSplitter(allPoses, restPose);
  INFO_LOG_FMT(VIDEO, "Detected {} distinct motion buckets.", frame_buckets.size());

  // 1. Group the animation into motions (Walk, Jump, Reach)
  std::vector<std::vector<Eigen::Matrix3Xd>> bucket_data;
  for (auto const& [id, frames] : frame_buckets)
  {
    bucket_data.push_back(GetPosesForFrames(allPoses, frames));
  }

  // 2. Generate initial "Dirty" shards (e.g. from the first bucket or all poses)
  auto initial_shards =
      FinalMergeProposals_Deterministic(restPose.cols(), allPoses, restPose, heat_data.F);

  int max_gpu_skinned_id = -1;
  auto resolved_assignments =
      ResolveHybridAssignments(gpu_skinned_ids, initial_shards, SVJ, &max_gpu_skinned_id);

  // 3. THE BUCKET MERGE: This will collapse the bicep clumps but keep the elbow
  auto final_grouping = BucketConsensusMerger(resolved_assignments, bucket_data, allPoses, restPose,
                                              heat_data.F, max_gpu_skinned_id, 0.015);

  // 6. PHASE SIX: Final Sanitization
  // Use your existing BFS "Island Cleaner" to ensure bones are contiguous.
  int final_bone_count = 0;
  for (int b : final_grouping)
    final_bone_count = std::max(final_bone_count, b + 1);

  CleanBoneIslands(final_grouping, heat_data.F, restPose.cols(), final_bone_count,
                   max_gpu_skinned_id);

  PrintMDEAnatomicalDiagnostic(final_grouping, allPoses, restPose, heat_data);

  return final_grouping;
}

/**
 * Validates the skeleton by rebuilding the animation using only bones and weights.
 *
 * This is the 'Proof of Work'. It applies the calculated Rigid Transforms (R, T)
 * to the Rest Pose vertices using the smooth Weights (W). If the result matches
 * the original animation, the bone discovery and skinning are successful.
 */
std::vector<std::vector<int>> ReconstructGroups(const std::vector<int>& cleanAssignments,
                                                int numBones)
{
  std::vector<std::vector<int>> groups(numBones);
  for (std::size_t vIdx = 0; vIdx < cleanAssignments.size(); ++vIdx)
  {
    int boneID = cleanAssignments[vIdx];
    if (boneID >= 0 && boneID < numBones)
    {
      groups[boneID].push_back(vIdx);
    }
  }
  return groups;
}

std::vector<GraphicsModEditor::VertexInfluence> FindTopInfluences(const Eigen::MatrixXd& W)
{
  const int N = static_cast<int>(W.rows());
  const int B = static_cast<int>(W.cols());
  std::vector<GraphicsModEditor::VertexInfluence> influences(N);

  // Noise Filter Threshold
  // Any bone with less than this influence is likely "mathematical noise"
  const double noise_floor = 0.02;

  for (int i = 0; i < N; ++i)
  {
    // 1. Collect all candidates and apply Noise Filter
    std::vector<std::pair<int, double>> pairs;
    for (int j = 0; j < B; ++j)
    {
      double weight = W(i, j);

      // If the influence is too weak, drop it entirely to stabilize the mesh
      if (weight < noise_floor)
      {
        weight = 0.0;
      }

      pairs.push_back({j, weight});
    }

    // 2. Sort descending
    std::sort(pairs.begin(), pairs.end(),
              [](const auto& a, const auto& b) { return a.second > b.second; });

    // 3. Extract Top 4 and sum them for re-normalization
    double sum = 0.0;
    for (int j = 0; j < 4; ++j)
    {
      if (j < (int)pairs.size() && pairs[j].second > 0.0)
      {
        influences[i].bone_ids[j] = pairs[j].first;
        influences[i].weights[j] = static_cast<float>(pairs[j].second);
        sum += pairs[j].second;
      }
      else
      {
        // Default to bone 0, weight 0.0 for empty slots
        influences[i].bone_ids[j] = 0;
        influences[i].weights[j] = 0.0f;
      }
    }

    // 4. Final Truncation Normalization
    // This is CRITICAL: After killing the noise, we must re-normalize
    // so the remaining "Strong" bones sum to 1.0.
    if (sum > 1e-6)
    {
      for (int j = 0; j < 4; ++j)
      {
        influences[i].weights[j] /= static_cast<float>(sum);
      }
    }
    else
    {
      // Fallback: If ALL weights were noise, snap to the strongest original bone
      // This prevents "invisible" vertices.
      int strongest_bone = pairs[0].first;
      influences[i].bone_ids[0] = strongest_bone;
      influences[i].weights[0] = 1.0f;
      for (int j = 1; j < 4; ++j)
      {
        influences[i].bone_ids[j] = 0;
        influences[i].weights[j] = 0.0f;
      }
    }
  }
  return influences;
}

/**
 * Connects bones into a skeletal tree.
 * 1. Finds the "most significant" bone (Torso) to act as Root.
 * 2. Uses Mesh Topology (Shared Edges) to build the primary tree.
 * 3. Uses Physical Distance as a fallback for floating/disconnected parts.
 */
std::vector<int> BuildBoneHierarchyForGLTF(const std::vector<int>& assignments,
                                           const Eigen::MatrixXi& F,
                                           const std::vector<Eigen::Vector3d>& boneCenters,
                                           int numBones)
{
  // --- 1. Count shared edges between bones ---
  Eigen::MatrixXd adj = Eigen::MatrixXd::Zero(numBones, numBones);
  for (int i = 0; i < F.rows(); ++i)
  {
    for (int j = 0; j < 3; ++j)
    {
      int u = assignments[F(i, j)];
      int v = assignments[F(i, (j + 1) % 3)];
      if (u >= 0 && v >= 0 && u != v)
      {
        adj(u, v)++;
        adj(v, u)++;
      }
    }
  }

  // --- 2. Find the Root Bone (The one with the most vertices) ---
  std::vector<int> boneSizes(numBones, 0);
  for (int b : assignments)
    if (b >= 0)
      boneSizes[b]++;

  int rootBone = 0;
  int maxVerts = -1;
  for (int i = 0; i < numBones; ++i)
  {
    if (boneSizes[i] > maxVerts)
    {
      maxVerts = boneSizes[i];
      rootBone = i;
    }
  }

  // --- 3. Build the Tree (Prim's Algorithm / MST) ---
  std::vector<int> parents(numBones, -1);
  std::vector<bool> visited(numBones, false);
  visited[rootBone] = true;

  for (int iteration = 0; iteration < numBones - 1; ++iteration)
  {
    int bestU = -1;  // Existing parent
    int bestV = -1;  // New child
    double maxShared = -1.0;

    // First pass: Try to connect via shared mesh edges (Topological)
    for (int u = 0; u < numBones; ++u)
    {
      if (!visited[u])
        continue;
      for (int v = 0; v < numBones; ++v)
      {
        if (visited[v])
          continue;
        if (adj(u, v) > maxShared)
        {
          maxShared = adj(u, v);
          bestU = u;
          bestV = v;
        }
      }
    }

    // Second pass: If no shared edges found, connect by distance (Geometric Fallback)
    // This handles floating eyes, belts, or disconnected props.
    if (bestV == -1 || maxShared <= 0)
    {
      double minDistance = std::numeric_limits<double>::max();
      for (int u = 0; u < numBones; ++u)
      {
        if (!visited[u])
          continue;
        for (int v = 0; v < numBones; ++v)
        {
          if (visited[v])
            continue;
          double dist = (boneCenters[u] - boneCenters[v]).norm();
          if (dist < minDistance)
          {
            minDistance = dist;
            bestU = u;
            bestV = v;
          }
        }
      }
    }

    if (bestV != -1)
    {
      parents[bestV] = bestU;
      visited[bestV] = true;
    }
  }

  return parents;
}

template <typename T>
int AddBufferToModel(tinygltf::Model& model, const std::vector<T>& data, int target, int type,
                     int componentType)
{
  size_t byteLength = data.size() * sizeof(T);
  size_t bufferOffset = model.buffers.empty() ? 0 : model.buffers[0].data.size();

  if (model.buffers.empty())
    model.buffers.emplace_back();
  model.buffers[0].data.resize(bufferOffset + byteLength);
  std::memcpy(model.buffers[0].data.data() + bufferOffset, data.data(), byteLength);

  tinygltf::BufferView view;
  view.buffer = 0;
  view.byteOffset = bufferOffset;
  view.byteLength = byteLength;
  view.target = target;
  model.bufferViews.push_back(view);

  tinygltf::Accessor accessor;
  accessor.bufferView = static_cast<int>(model.bufferViews.size() - 1);
  accessor.byteOffset = 0;
  accessor.componentType = componentType;

  if (type == TINYGLTF_TYPE_SCALAR)
  {
    accessor.count = data.size();
  }
  else if (type == TINYGLTF_TYPE_VEC3)
  {
    accessor.count = data.size() / 3;
  }
  else if (type == TINYGLTF_TYPE_VEC4)
  {
    accessor.count = data.size() / 4;
  }
  else if (type == TINYGLTF_TYPE_MAT4)
  {
    accessor.count = data.size() / 16;
  }

  accessor.type = type;
  model.accessors.push_back(accessor);

  return static_cast<int>(model.accessors.size() - 1);
}

void ExportToGLTF(const std::string& path, const Eigen::Matrix3Xd& V, const Eigen::MatrixXi& F,
                  const std::vector<Eigen::Vector3d>& boneCenters,
                  const std::vector<GraphicsModEditor::VertexInfluence>& influences)
{
  tinygltf::Model model;
  tinygltf::Asset asset;
  asset.version = "2.0";
  model.asset = asset;

  // 1. Prepare Data for Packing
  std::vector<float> posData;
  for (int i = 0; i < V.cols(); ++i)
  {
    posData.push_back((float)V(0, i));
    posData.push_back((float)V(1, i));
    posData.push_back((float)V(2, i));
  }

  std::vector<uint32_t> indexData;
  for (int i = 0; i < F.rows(); ++i)
  {
    indexData.push_back((uint32_t)F(i, 0));
    indexData.push_back((uint32_t)F(i, 1));
    indexData.push_back((uint32_t)F(i, 2));
  }

  std::vector<u16> bone_data;
  std::vector<float> weight_data;
  for (const auto& inf : influences)
  {
    for (int j = 0; j < 4; ++j)
    {
      bone_data.push_back(static_cast<u16>(inf.bone_ids[j]));
      weight_data.push_back(inf.weights[j]);
    }
  }

  // 2. Pack Buffers
  const int posAcc =
      AddBufferToModel(model, posData, 34962, TINYGLTF_TYPE_VEC3, TINYGLTF_COMPONENT_TYPE_FLOAT);
  const int idxAcc = AddBufferToModel(model, indexData, 34963, TINYGLTF_TYPE_SCALAR,
                                      TINYGLTF_COMPONENT_TYPE_UNSIGNED_INT);
  const int jntAcc = AddBufferToModel(model, bone_data, 34962, TINYGLTF_TYPE_VEC4,
                                      TINYGLTF_COMPONENT_TYPE_UNSIGNED_SHORT);
  const int wgtAcc = AddBufferToModel(model, weight_data, 34962, TINYGLTF_TYPE_VEC4,
                                      TINYGLTF_COMPONENT_TYPE_FLOAT);

  std::vector<float> ibmData;
  for (const auto& center : boneCenters)
  {
    // A matrix that translates by -center
    Eigen::Matrix4d invBind = Eigen::Matrix4d::Identity();
    invBind(0, 3) = -center.x();
    invBind(1, 3) = -center.y();
    invBind(2, 3) = -center.z();

    // Pack 16 floats (column-major)
    for (int col = 0; col < 4; ++col)
    {
      for (int row = 0; row < 4; ++row)
      {
        ibmData.push_back((float)invBind(row, col));
      }
    }
  }

  // 3. Build Mesh
  tinygltf::Mesh mesh;
  tinygltf::Primitive prim;
  prim.attributes["POSITION"] = posAcc;
  prim.attributes["JOINTS_0"] = jntAcc;
  prim.attributes["WEIGHTS_0"] = wgtAcc;
  prim.indices = idxAcc;
  prim.mode = 4;  // Triangles
  mesh.primitives.push_back(prim);
  model.meshes.push_back(mesh);

  // 4. Create Bone Nodes and Skin
  tinygltf::Skin skin;

  const int ibmAcc =
      AddBufferToModel(model, ibmData, 0, TINYGLTF_TYPE_MAT4, TINYGLTF_COMPONENT_TYPE_FLOAT);
  skin.inverseBindMatrices = ibmAcc;

  tinygltf::Scene scene;
  tinygltf::Node meshNode;
  meshNode.mesh = 0;
  meshNode.skin = 0;
  model.nodes.push_back(meshNode);
  scene.nodes.push_back(0);

  for (std::size_t i = 0; i < boneCenters.size(); ++i)
  {
    tinygltf::Node bone;
    bone.name = "Bone_" + std::to_string(i);
    bone.translation = {boneCenters[i].x(), boneCenters[i].y(), boneCenters[i].z()};
    model.nodes.push_back(bone);

    int boneNodeIdx = static_cast<int>(model.nodes.size());
    model.nodes.push_back(bone);
    skin.joints.push_back(boneNodeIdx);
    scene.nodes.push_back(boneNodeIdx);
  }
  model.skins.push_back(skin);

  model.scenes.push_back(scene);
  model.defaultScene = 0;

  // 5. Save
  tinygltf::TinyGLTF writer;
  writer.WriteGltfSceneToFile(&model, path, true, true, true, false);
}

void ExportRigBinary(
    File::IOFile* file_data,
    const Eigen::MatrixXd& V_welded,    // Unique Positions [3 x N]
    const Eigen::VectorXi& global_SVJ,  // Map: UnweldedIdx -> WeldedIdx
    const std::vector<GraphicsModEditor::VertexInfluence>& vertex_influences,
    const std::vector<Eigen::Vector3d>& bone_centers,
    const std::map<GraphicsModSystem::DrawCallID, GraphicsModEditor::VertexGroupDumper::FinalData>&
        draw_call_to_data,
    const DrawToSkinningIdToGlobalBone& draw_call_to_skinning_details)
{
  GraphicsModEditor::ExporterSkinningRig export_rig;

  // 2. Bone Centers & Weights
  export_rig.global_weights = vertex_influences;
  for (const auto& bc_double : bone_centers)
  {
    export_rig.runtime_rig.bone_rest_centers.push_back(bc_double.cast<float>());
  }

  // 3. Chunk-Specific Slicing (SVJ + Bone Groups)
  std::size_t global_v_offset = 0;
  for (auto const& [draw_call, data] : draw_call_to_data)
  {
    const u32 v_count = static_cast<u32>(data.original_vertex_count);

    VideoCommon::ChunkRigData& chunk_rig = export_rig.runtime_rig.draw_call_rig_details[draw_call];
    chunk_rig.draw_call_id = draw_call;

    if (const auto iter = draw_call_to_skinning_details.find(draw_call);
        iter != draw_call_to_skinning_details.end())
    {
      for (const int skinning_id : data.gpu_skinning_ids)
      {
        if (const auto id_iter = iter->second.find(skinning_id); id_iter != iter->second.end())
        {
          if (const auto inverse_iter = data.gpu_skinning_inverse_transforms.find(skinning_id);
              inverse_iter != data.gpu_skinning_inverse_transforms.end())
          {
            VideoCommon::GpuSkinnedData skinning_data;
            skinning_data.global_bone_id = id_iter->second;
            skinning_data.inverse_transform = inverse_iter->second;
            chunk_rig.gpu_skinned_data[skinning_id] = skinning_data;
          }
        }
      }
    }
    else
    {
      // --- PART A: Local SVJ (Native -> Welded Map) ---
      auto& local_svj = export_rig.draw_call_to_local_svj[draw_call];
      local_svj.reserve(v_count);
      for (u32 i = 0; i < v_count; ++i)
      {
        // global_SVJ is a VectorXi, so we just use () or []
        local_svj.push_back(global_SVJ(global_v_offset + i));
      }

      // --- PART B: SVD Tracking Groups (ChunkRigData) ---
      // This tells the SVD solver: "In this chunk, which vertices belong to which bone?"
      std::unordered_map<int, VideoCommon::LocalBoneGroup> aggregator;
      for (u32 local_i = 0; local_i < v_count; ++local_i)
      {
        int welded_idx = local_svj[local_i];
        const GraphicsModEditor::VertexInfluence& inf = vertex_influences[welded_idx];

        // Check all 4 baked influences for this vertex
        for (int k = 0; k < 4; ++k)
        {
          float w = inf.weights[k];
          // Only track vertices with significant weight to keep SVD stable
          if (w > 0.1f)
          {
            int b_id = inf.bone_ids[k];
            auto& group = aggregator[b_id];
            group.global_bone_id = b_id;

            // PAIR CORRESPONDENCE:
            // We need the moving vertex (local_i) and the rest vertex (welded_idx)
            group.original_indices.push_back(static_cast<int>(local_i));
            group.welded_indices.push_back(welded_idx);
            group.weights.push_back(w);  // Confidence score for SVD
          }
        }
      }

      for (auto& [id, group] : aggregator)
      {
        chunk_rig.bones.push_back(std::move(group));
      }
    }

    global_v_offset += v_count;
  }

  GraphicsModEditor::ExporterSkinningRig::ToBinary(file_data, export_rig);
}

}  // namespace

namespace GraphicsModEditor
{
bool VertexGroupDumper::IsDrawCallInRecording(GraphicsModSystem::DrawCallID draw_call_id) const
{
  if (!m_recording_request.has_value())
    return false;

  return m_recording_request->m_draw_call_ids.contains(draw_call_id);
}

void VertexGroupDumper::AddDataToRecording(GraphicsModSystem::DrawCallID draw_call_id,
                                           const GraphicsModSystem::DrawDataView& draw_data)
{
  auto current_vertex_positions = ReadMeshPositions(draw_data);

  Eigen::Matrix3Xd matrix_positions = ConvertMeshPositions(current_vertex_positions, draw_data);
  auto& data = m_draw_call_to_data[draw_call_id];

  if (IsDuplicateMesh(matrix_positions, data.mesh_poses))
    return;

  if (data.original_vertex_count == 0)
  {
    data.original_vertex_count = current_vertex_positions.size();

    if (data.original_vertex_count == 0)
      return;

    data.gpu_skinning_ids = GetGPUSkinningIDs(draw_data);
    for (const int gpu_skin_id : data.gpu_skinning_ids)
    {
      const auto [iter, added] = data.gpu_skinning_inverse_transforms.try_emplace(
          gpu_skin_id, Common::Matrix44::Identity());
      if (added)
      {
        Common::Matrix44& position_transform = iter->second;

        // Extract the GPU skinning position transform for this index
        for (std::size_t i = 0; i < 3; i++)
        {
          position_transform.data[i * 4 + 0] =
              draw_data.gpu_skinning_position_transform[gpu_skin_id + i][0];
          position_transform.data[i * 4 + 1] =
              draw_data.gpu_skinning_position_transform[gpu_skin_id + i][1];
          position_transform.data[i * 4 + 2] =
              draw_data.gpu_skinning_position_transform[gpu_skin_id + i][2];
          position_transform.data[i * 4 + 3] =
              draw_data.gpu_skinning_position_transform[gpu_skin_id + i][3];
        }
        position_transform.data[12] = 0;
        position_transform.data[13] = 0;
        position_transform.data[14] = 0;
        position_transform.data[15] = 1;

        position_transform = position_transform.Inverted();
      }
    }

    if (draw_data.uid->rasterization_state.primitive == PrimitiveType::TriangleStrip)
    {
      std::vector<int> indices;
      u32 strip_count = 0;
      for (std::size_t i = 0; i < draw_data.index_data.size(); i++)
      {
        // Primitive restart
        if (draw_data.index_data[i] == UINT16_MAX)
        {
          strip_count = 0;  // Reset strip state
          continue;
        }

        // A triangle is at least 3 verts
        strip_count++;
        if (strip_count < 3)
          continue;

        u16 i0, i1, i2;

        // Triangle index 'n' in the current strip is at (i-2, i-1, i)
        // Its local index is (triangleCountInStrip - 3)
        if ((strip_count - 3) % 2 == 0)
        {
          i0 = draw_data.index_data[i - 2];
          i1 = draw_data.index_data[i - 1];
          i2 = draw_data.index_data[i];
        }
        else
        {
          // Swap for odd-numbered triangles to maintain consistent CCW winding
          i0 = draw_data.index_data[i - 2];
          i1 = draw_data.index_data[i];
          i2 = draw_data.index_data[i - 1];
        }

        if (i0 == i1 || i1 == i2 || i0 == i2)
        {
          continue;
        }

        indices.push_back(i0);
        indices.push_back(i1);
        indices.push_back(i2);
      }

      const std::size_t num_triangles = indices.size() / 3;
      Eigen::MatrixXi F(num_triangles, 3);

      for (int i = 0; i < num_triangles; ++i)
      {
        F(i, 0) = indices[i * 3 + 0];
        F(i, 1) = indices[i * 3 + 1];
        F(i, 2) = indices[i * 3 + 2];
      }
      data.rest_pose_face_indexes = std::move(F);
    }
    else
    {
      const std::size_t num_triangles = draw_data.index_data.size() / 3;
      Eigen::MatrixXi F(num_triangles, 3);

      for (int i = 0; i < num_triangles; ++i)
      {
        F(i, 0) = draw_data.index_data[i * 3 + 0];
        F(i, 1) = draw_data.index_data[i * 3 + 1];
        F(i, 2) = draw_data.index_data[i * 3 + 2];
      }

      data.rest_pose_face_indexes = std::move(F);
    }
  }

  m_current_xfb_data.emplace_back(draw_call_id, std::move(matrix_positions));
}

void VertexGroupDumper::OnXFBCreated(const std::string& hash)
{
  if (!m_recording_request.has_value())
    return;

  m_xfb_to_draw_data[hash] = std::move(m_current_xfb_data);
}

void VertexGroupDumper::OnFramePresented(std::span<std::string> xfbs_presented)
{
  if (m_xfb_to_draw_data.empty())
    return;

  for (const auto& xfb : xfbs_presented)
  {
    if (const auto iter = m_xfb_to_draw_data.find(xfb); iter != m_xfb_to_draw_data.end())
    {
      for (auto& [draw_call, data] : iter->second)
      {
        auto& final_data = m_draw_call_to_data[draw_call];
        final_data.mesh_poses.push_back(std::move(data));
        final_data.frame_id_to_mesh_index[m_frame_id] = final_data.mesh_poses.size() - 1;
      }
      m_xfb_to_draw_data.erase(iter);
    }
  }

  // If our recording is done and we have no data left to capture
  if (m_xfb_to_draw_data.empty() && !m_recording_request.has_value())
  {
    Save();
    m_draw_call_to_data.clear();
    m_frame_id = 0;
    return;
  }

  m_frame_id++;
}

void VertexGroupDumper::Record(const VertexGroupRecordingRequest& request)
{
  m_recording_request = request;
}

void VertexGroupDumper::StopRecord()
{
  m_recording_request.reset();
}

bool VertexGroupDumper::IsRecording() const
{
  return m_recording_request.has_value();
}

void VertexGroupDumper::Save()
{
  if (m_draw_call_to_data.empty()) [[unlikely]]
    return;

  Eigen::MatrixXi F_global;
  std::size_t v_offset = 0;
  for (const auto& [draw_call, data] : m_draw_call_to_data)
  {
    F_global.conservativeResize(F_global.rows() + data.rest_pose_face_indexes.rows(), 3);
    F_global.bottomRows(data.rest_pose_face_indexes.rows()) =
        data.rest_pose_face_indexes.array() + v_offset;
    v_offset += data.original_vertex_count;  // Number of vertices in THIS chunk
  }

  auto reassembled_data = ReassembleSignificantFrames(m_draw_call_to_data);

  std::vector<GraphicsModSystem::DrawCallID> vertex_to_draw_call;
  for (const auto& [draw_call, data] : m_draw_call_to_data)
  {
    const int num_verts = static_cast<int>(data.mesh_poses[0].cols());

    for (int j = 0; j < num_verts; ++j)
    {
      vertex_to_draw_call.push_back(draw_call);
    }
  }

  double mesh_scale;
  Eigen::Vector3d mesh_centroid;
  NormalizeWeldedAnimation(reassembled_data, mesh_scale, mesh_centroid);

  Eigen::MatrixXd V_raw = reassembled_data[0].transpose();  // Nx3

  // 2. Find the Gap (make sure this is correct)
  const double weld_distance = FindMinimumChunkGap(reassembled_data[0], vertex_to_draw_call);

  ERROR_LOG_FMT(VIDEO, "Weld distance: {}", weld_distance);

  // 3. WELD EVERYTHING
  Eigen::MatrixXd SV;  // Unique Vertices
  Eigen::VectorXi SVI, SVJ;
  Eigen::MatrixXi SF;
  double final_epsilon = std::max(0.001, weld_distance);
  weld_mesh_euclidean(reassembled_data[0].transpose(), F_global, final_epsilon, SV, SVI, SVJ, SF);

  WARN_LOG_FMT(VIDEO, "[MeshDebug] Welded vertex count: {} | Welded face count: {}", SV.rows(),
               SF.rows());

  for (auto& pose : reassembled_data)
  {
    Eigen::Matrix3Xd p = Eigen::Matrix3Xd::Zero(3, SV.rows());
    Eigen::VectorXd counts = Eigen::VectorXd::Zero(SV.rows());
    for (int i = 0; i < pose.cols(); ++i)
    {
      int uIdx = SVJ(i);
      p.col(uIdx) += pose.col(i);
      counts(uIdx) += 1.0;
    }
    for (int i = 0; i < SV.rows(); ++i)
      if (counts(i) > 0)
        p.col(i) /= counts(i);
    pose = p;
  }

  SimpleGeodesic::HeatGeodesicsData heat_data;
  if (!SimpleGeodesic::heat_geodesics_precompute(reassembled_data[0].transpose(), SF, heat_data))
  {
    ERROR_LOG_FMT(VIDEO, "Failed to precompute heat data!");
    return;
  }

  // Get the global sim IDs from the UN-WELDED stitched buffer
  DrawToSkinningIdToGlobalBone draw_to_skinning_details;
  const auto stitched_gpu_skinning_ids =
      GetGlobalGPUSkinningIDs(m_draw_call_to_data, &draw_to_skinning_details);

  auto clean_grouping = GenerateRigFromMotion(reassembled_data, reassembled_data[0], heat_data,
                                              stitched_gpu_skinning_ids, SVJ);

  int bone_count = 0;
  for (int id : clean_grouping)
  {
    bone_count = std::max(bone_count, id + 1);
  }

  const std::string path_prefix =
      File::GetUserPath(D_DUMP_IDX) + SConfig::GetInstance().GetGameID();

  const std::time_t start_time = std::time(nullptr);
  const auto local_time = Common::LocalTime(start_time);
  if (!local_time)
    return;

  const std::string base_name = fmt::format("{}_{:%Y-%m-%d_%H-%M-%S}", path_prefix, *local_time);

  const auto weights = ComputeHarmonicWeights(clean_grouping, reassembled_data[0], SF,
                                              reassembled_data, heat_data, bone_count);
  const auto influences = FindTopInfluences(weights);

  const auto bone_groups = ReconstructGroups(clean_grouping, bone_count);
  const auto bone_centers =
      ComputeBoneRestPositions(bone_groups, reassembled_data, reassembled_data[0]);
  ExportToGLTF(fmt::format("{}_bones.gltf", base_name), SV.transpose(), SF, bone_centers,
               influences);

  File::IOFile outbound_file(fmt::format("{}.rig", base_name), "wb");
  ExportRigBinary(&outbound_file, reassembled_data[0], SVJ, influences, bone_centers,
                  m_draw_call_to_data, draw_to_skinning_details);
}
}  // namespace GraphicsModEditor
