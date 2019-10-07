#include "product/block_cluster_tree.h"
#include "utils.h"

namespace LWS {

    long BlockClusterTree::illSepTime = 0;
    long BlockClusterTree::wellSepTime = 0;
    long BlockClusterTree::traversalTime = 0;

    BlockClusterTree::BlockClusterTree(PolyCurveGroup* cg, BVHNode3D* tree, double sepCoeff, double a, double b, double e) {
        curves = cg;
        alpha = a;
        beta = b;
        separationCoeff = sepCoeff;
        epsilon = e;

        tree_root = tree;
        ClusterPair pair{tree, tree};
        unresolvedPairs.push_back(pair);

        nVerts = curves->NumVertices();
        constraintsSet = false;

        while (unresolvedPairs.size() > 0) {
            splitInadmissibleNodes();
        }

        mode = BlockTreeMode::MatrixOnly;
    }

    void BlockClusterTree::splitInadmissibleNodes() {
        std::vector<ClusterPair> nextPairs;

        for (ClusterPair pair : unresolvedPairs) {
            if (pair.cluster1->NumElements() == 0 || pair.cluster2->NumElements() == 0) {
                // Drop pairs where one of the sides has 0 vertices
                continue;
            }
            else if (pair.cluster1->NumElements() == 1 && pair.cluster2->NumElements() == 1) {
                // If this is two singleton vertices, put in the inadmissible list
                // so they get multiplied accurately
                inadmissiblePairs.push_back(pair);
            }
            else if (isPairAdmissible(pair, separationCoeff)) {
                // If the pair is admissible, mark it as such and leave it
                admissiblePairs.push_back(pair);
            }
            else if (isPairSmallEnough(pair)) {
                inadmissiblePairs.push_back(pair);
            }
            else {
                // Otherwise, subdivide it into child pairs
                for (size_t i = 0; i < pair.cluster1->children.size(); i++) {
                    for (size_t j = 0; j < pair.cluster2->children.size(); j++) {
                        ClusterPair pair_ij{pair.cluster1->children[i], pair.cluster2->children[j]};
                        nextPairs.push_back(pair_ij);
                    }
                }
            }
        }
        // Replace the inadmissible pairs by the next set
        unresolvedPairs.clear();
        unresolvedPairs = nextPairs;
    }

    bool BlockClusterTree::isPairSmallEnough(ClusterPair pair) {
        int s1 = pair.cluster1->NumElements();
        int s2 = pair.cluster2->NumElements();
        return (s1 <= 1) || (s2 <= 1) || (s1 + s2 <= 8);
    }

    bool BlockClusterTree::isPairAdmissible(ClusterPair pair, double theta) {
        if (pair.cluster1 == pair.cluster2) return false;

        Vector2 c1spread = pair.cluster1->viewspaceBounds(pair.cluster2->centerOfMass);
        Vector2 c2spread = pair.cluster2->viewspaceBounds(pair.cluster1->centerOfMass);

        double maxRadial = fmax(c1spread.x, c2spread.x);
        double maxLinear = fmax(c1spread.y, c2spread.y);

        double distance = norm(pair.cluster1->centerOfMass - pair.cluster2->centerOfMass);

        bool isAdm = fmax(maxRadial, maxLinear) < theta * distance;
        return isAdm;
    }

    void BlockClusterTree::PrintData() {
        std::cout << admissiblePairs.size() << " admissible pairs" << std::endl;
        std::cout << inadmissiblePairs.size() << " inadmissible pairs" << std::endl;
    }

    void BlockClusterTree::SetBlockTreeMode(BlockTreeMode m) {
        mode = m;
    }

    void BlockClusterTree::MultiplyAdmissible(Eigen::MatrixXd &v_hat, Eigen::MatrixXd &b_hat) const {
        for (ClusterPair pair : admissiblePairs) {
            AfApproxProduct(pair, v_hat, b_hat);
        }
    }

    void BlockClusterTree::MultiplyAdmissibleFast(Eigen::MatrixXd &v_hat, Eigen::MatrixXd &b_hat) const {
        int nVerts = curves->NumVertices();
        Eigen::VectorXd b_tilde;
        b_tilde.setZero(nVerts);

        Eigen::VectorXd ones;
        ones.setOnes(nVerts);
        Eigen::VectorXd h_f = MultiplyAf(ones);

        for (int i = 0; i < 3; i++) {
            Eigen::VectorXd v_col = v_hat.col(i);
            b_hat.col(i) = MultiplyAf(v_col);
        }

        b_hat = 2 * (h_f.asDiagonal() * v_hat - b_hat);
    }

    void BlockClusterTree::MultiplyInadmissible(Eigen::MatrixXd &v_hat, Eigen::MatrixXd &b_hat) const {
        for (ClusterPair pair : inadmissiblePairs) {
            AfFullProduct(pair, v_hat, b_hat);
        }
    }

    Eigen::VectorXd BlockClusterTree::MultiplyAf(Eigen::VectorXd &v) const {
        tree_root->recursivelyZeroMVFields();
        Eigen::VectorXd result(v.rows());
        result.setZero();
        SetVIs(tree_root, v);
        SetBIs(tree_root, result);
        result = tree_root->fullMasses.asDiagonal() * result;
        return result;
    }
    
    void BlockClusterTree::SetVIs(BVHNode3D* node, Eigen::VectorXd &v_hat) const {
        if (node->IsLeaf()) {
            int index = node->VertexIndex();
            double w_j = node->totalMass;
            double v_hat_j = v_hat(index);
            node->V_I = w_j * v_hat_j;
        }
        else {
            node->V_I = 0;
            // Start at the roots and propagate upward
            for (BVHNode3D* child : node->children) {
                SetVIs(child, v_hat);
                node->V_I += child->V_I;
            }
        }
    }

    void BlockClusterTree::SetBIs(BVHNode3D* node, Eigen::VectorXd &b_tilde) const {
        // First accumulate the sums of a_IJ * V_J from admissible cluster pairs
        for (ClusterPair pair : admissiblePairs) {
            double pow_s = beta - alpha;
            double a_IJ = 1.0 / pow(norm(pair.cluster1->centerOfMass - pair.cluster2->centerOfMass), pow_s);
            pair.cluster1->aIJ_VJ += a_IJ * pair.cluster2->V_I;
        }

        // Now recursively propagate downward
        PropagateBIs(node, 0, b_tilde);
    }

    void BlockClusterTree::PropagateBIs(BVHNode3D* node, double parent_BI, Eigen::VectorXd &b_tilde) const {
        node->B_I = parent_BI + node->aIJ_VJ;
        if (node->IsLeaf()) {
            b_tilde(node->VertexIndex()) = node->B_I;
        }
        else {
            for (BVHNode3D* child : node->children) {
                PropagateBIs(child, node->B_I, b_tilde);
            }
        }
    }

    void BlockClusterTree::AfFullProduct(ClusterPair pair, Eigen::MatrixXd &v_hat, Eigen::MatrixXd &result) const
    {
        double pow_s = (beta - alpha);

        std::vector<double> a_times_one(pair.cluster1->clusterIndices.size());
        std::vector<Vector3> a_times_v(pair.cluster1->clusterIndices.size());
        
        for (size_t i = 0; i < pair.cluster1->clusterIndices.size(); i++) {
            int e1index = pair.cluster1->clusterIndices[i];

            for (size_t j = 0; j < pair.cluster2->clusterIndices.size(); j++) {
                int e2index = pair.cluster2->clusterIndices[j];

                PointOnCurve p1 = curves->GetCurvePoint(e1index);
                PointOnCurve p2 = curves->GetCurvePoint(e2index);

                bool isNeighbors = (p1 == p2 || p1.Next() == p2 || p1 == p2.Next() || p1.Next() == p2.Next());
                
                Vector3 mid1 = (p1.Position() + p1.Next().Position()) / 2;
                Vector3 mid2 = (p2.Position() + p2.Next().Position()) / 2;

                double l1 = norm(p1.Position() - p1.Next().Position());
                double l2 = norm(p2.Position() - p2.Next().Position());

                double af_ij = (isNeighbors) ? 0 :
                    (l1 * l2) / pow(norm(mid1 - mid2), pow_s);

                // We dot this row of Af(i, j) with the all-ones vector, which means we
                // just add up all entries of that row.
                a_times_one[i] += af_ij;

                // We also dot it with v_hat(J).
                a_times_v[i] += af_ij * SelectRow(v_hat, e2index);
            }

            // We've computed everything from row i now, so add to the results vector
            Vector3 toAdd = 2 * (a_times_one[i] * SelectRow(v_hat, e1index) - a_times_v[i]);
            AddToRow(result, e1index, toAdd);
        }
    }
    
    void BlockClusterTree::AfApproxProduct(ClusterPair pair, Eigen::MatrixXd &v_hat, Eigen::MatrixXd &result) const
    {
        long traversalStart = Utils::currentTimeMilliseconds();
        Eigen::VectorXd wf_i;
        pair.cluster1->fillClusterMassVector(wf_i);
        Eigen::VectorXd wf_j;
        pair.cluster2->fillClusterMassVector(wf_j);
        long traversalEnd = Utils::currentTimeMilliseconds();

        traversalTime += (traversalEnd - traversalStart);
        
        double pow_s = beta - alpha;
        double a_IJ = 1.0 / pow(norm(pair.cluster1->centerOfMass - pair.cluster2->centerOfMass), pow_s);
        // Evaluate a(I,J) * w_f(J)^T * 1(J)
        double a_wf_1 = a_IJ * wf_j.sum();

        // Evaluate a(I,J) * w_f(J)^T * v_hat(J)
        Vector3 a_wf_J{0, 0, 0};
        // Dot w_f(J) with v_hat(J)
        for (int j = 0; j < wf_j.rows(); j++) {
            a_wf_J += wf_j(j) * SelectRow(v_hat, pair.cluster2->clusterIndices[j]);
        }
        a_wf_J *= a_IJ;

        // Add in the results
        for (int i = 0; i < wf_i.rows(); i++) {
            Vector3 toAdd = wf_i[i] * 2 * (a_wf_1 * SelectRow(v_hat, pair.cluster1->clusterIndices[i]) - a_wf_J);
            AddToRow(result, pair.cluster1->clusterIndices[i], toAdd);
        }
    }

    void BlockClusterTree::CompareBlocks() {
        Eigen::MatrixXd fullBlock;
        Eigen::MatrixXd approxBlock;

        double totalError = 0;
        double totalNorm = 0;

        for (ClusterPair pair : inadmissiblePairs) {
            // Inadmissible blocks are computed exactly, so no error contribution
            fullBlock = AfFullBlock(pair);

            double normFull = fullBlock.norm();
            totalNorm += normFull * normFull;
        }
        for (ClusterPair pair : admissiblePairs) {
            fullBlock = AfFullBlock(pair);
            approxBlock = AfApproxBlock(pair);

            double normFull = fullBlock.norm();
            double normDiff = (fullBlock - approxBlock).norm();
            double relative = 100 * normDiff / normFull;

            if (relative > 50) {
                std::cout << "(" << pair.cluster1->NumElements() << ", " << pair.cluster2->NumElements() << ")" << std::endl;
                std::cout << "Full:\n" << fullBlock << std::endl;
                std::cout << "Approx:\n" << approxBlock << std::endl;
                std::cout << "Error: " << normDiff << " (" << relative << " percent)" << std::endl;
            }

            totalNorm += normFull * normFull;
            totalError += normDiff * normDiff;
        }

        totalError = sqrt(totalError);
        totalNorm = sqrt(totalNorm);
        double totalRelative = 100 * totalError / totalNorm;

        std::cout << "Total error = " << totalError << " (" << totalRelative << " percent; total norm = " << totalNorm << ")" << std::endl;
    }

    Eigen::MatrixXd BlockClusterTree::AfFullBlock(ClusterPair pair) {
        std::vector<VertexBody6D> children1;
        pair.cluster1->accumulateChildren(children1);
        std::vector<VertexBody6D> children2;
        pair.cluster2->accumulateChildren(children2);
        
        double pow_s = beta - alpha;
        Eigen::MatrixXd block;
        block.setZero(children1.size(), children2.size());

        for (size_t i = 0; i < children1.size(); i++) {
            for (size_t j = 0; j < children2.size(); j++) {
                PointOnCurve p1 = curves->GetCurvePoint(children1[i].vertIndex1);
                PointOnCurve p2 = curves->GetCurvePoint(children2[j].vertIndex1);

                bool isNeighbors = (p1 == p2 || p1.Next() == p2 || p1 == p2.Next() || p1.Next() == p2.Next());

                double w_i = children1[i].mass;
                double w_j = children2[j].mass;

                Vector3 c_i = children1[i].pt.position;
                Vector3 c_j = children2[j].pt.position;

                block(i, j) = (isNeighbors) ? 0 : -w_i * w_j / pow(norm(c_i - c_j), pow_s);
            }
        }

        return block;
    }

    Eigen::MatrixXd BlockClusterTree::AfApproxBlock(ClusterPair pair) {
        std::vector<VertexBody6D> children1;
        pair.cluster1->accumulateChildren(children1);
        std::vector<VertexBody6D> children2;
        pair.cluster2->accumulateChildren(children2);
        
        double pow_s = beta - alpha;

        std::vector<double> wf_i(children1.size());
        std::vector<double> wf_j(children2.size());

        for (size_t i = 0; i < children1.size(); i++) {
            wf_i[i] = children1[i].mass;
        }

        for (size_t j = 0; j < children2.size(); j++) {
            wf_j[j] = children2[j].mass;
        }
        double a_IJ = 1.0 / pow(norm(pair.cluster1->centerOfMass - pair.cluster2->centerOfMass), pow_s);

        Eigen::MatrixXd block;
        block.setZero(children1.size(), children2.size());

        for (size_t i = 0; i < children1.size(); i++) {
            for (size_t j = 0; j < children2.size(); j++) {
                block(i, j) = -wf_i[i] * a_IJ * wf_j[j];
            }
        }

        return block;
    }
}