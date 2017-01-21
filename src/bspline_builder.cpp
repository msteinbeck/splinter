/*
 * This file is part of the SPLINTER library.
 * Copyright (C) 2012 Bjarne Grimstad (bjarne.grimstad@gmail.com).
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/.
*/

#include "bspline_builder.h"
#include "kronecker_product.h"
#include "unsupported/Eigen/KroneckerProduct"
#include <knot_utils.h>
#include <linear_solvers.h>
#include <serializer.h>
#include <iostream>
#include <utilities.h>

namespace SPLINTER
{
// Default constructor
BSpline::Builder::Builder(unsigned int dim_x, unsigned int dim_y)
        : _dim_x(dim_x),
          _dim_y(dim_y),
          _degrees(getBSplineDegrees(_dim_x, 3)),
          _numBasisFunctions(std::vector<unsigned int>(_dim_x, 1)),
          _knotSpacing(KnotSpacing::AS_SAMPLED)
{
}

/*
 * Fit B-spline to data
 */
BSpline BSpline::Builder::fit(const DataTable &data, Smoothing smoothing, double alpha) const
{
    if (data.getDimX() != _dim_x)
        throw Exception("BSpline::Builder::fit: Expected " + std::to_string(_dim_x) + " input variables.");

    if (data.getDimY() != _dim_y)
        throw Exception("BSpline::Builder::fit: Expected " + std::to_string(_dim_y) + " output variables.");

    if (alpha < 0)
        throw Exception("BSpline::Builder::fit: alpha must be non-negative.");

#ifndef NDEBUG
    if (!data.isGridComplete())
        std::cout << "BSpline::Builder::fit: Building B-spline from irregular (incomplete) grid." << std::endl;
#endif // NDEBUG

    // Build knot vectors
    auto knotVectors = computeKnotVectors(data);

    // Build B-spline (with default coefficients)
    auto bspline = BSpline(_dim_x, _dim_y, knotVectors, _degrees);

    // Compute coefficients from samples and update B-spline
    auto coefficients = computeControlPoints(bspline, data, smoothing, alpha);
    bspline.setControlPoints(coefficients);

    return bspline;
}

/*
 * Find coefficients of B-spline by solving:
 * min ||A*x - b||^2 + alpha*||R||^2,
 * where
 * A = mxn matrix of n basis functions evaluated at m sample points,
 * b = vector of m sample points y-values (or x-values when calculating knot averages),
 * x = B-spline coefficients (or knot averages),
 * R = Regularization matrix,
 * alpha = regularization parameter.
 */
DenseMatrix BSpline::Builder::computeControlPoints(const BSpline &bspline,
                                                   const DataTable &data,
                                                   Smoothing smoothing,
                                                   double alpha) const
{
    SparseMatrix B = computeBasisFunctionMatrix(bspline, data);
    SparseMatrix A = B;
    DenseMatrix b = stackSamplePointValues(data);

    if (smoothing == Smoothing::IDENTITY)
    {
        /*
         * Computing B-spline coefficients with a regularization term
         * ||Ax-b||^2 + alpha*x^T*x
         *
         * NOTE: This corresponds to a Tikhonov regularization (or ridge regression) with the Identity matrix.
         * See: https://en.wikipedia.org/wiki/Tikhonov_regularization
         *
         * NOTE2: consider changing regularization factor to (alpha/numSample)
         */
        SparseMatrix Bt = B.transpose();
        A = Bt*B;
        b = Bt*b;

        auto I = SparseMatrix(A.cols(), A.cols());
        I.setIdentity();
        A += alpha*I;
    }
    else if (smoothing == Smoothing::PSPLINE)
    {
        /*
         * The P-Spline is a smooting B-spline which relaxes the interpolation constraints on the control points to allow
         * smoother spline curves. It minimizes an objective which penalizes both deviation from sample points (to lower bias)
         * and the magnitude of second derivatives (to lower variance).
         *
         * Setup and solve equations Ax = b,
         * A = B'*W*B + l*D'*D
         * b = B'*W*y
         * x = control coefficients or knot averages.
         * B = basis functions at sample x-values,
         * W = weighting matrix for interpolating specific points
         * D = second-order finite difference matrix
         * l = penalizing parameter (increase for more smoothing)
         * y = sample y-values when calculating control coefficients,
         * y = sample x-values when calculating knot averages
         */

        // Assuming regular grid
        unsigned int numSamples = data.getNumSamples();

        SparseMatrix Bt = B.transpose();

        // Weight matrix
        SparseMatrix W;
        W.resize(numSamples, numSamples);
        W.setIdentity();

        // Second order finite difference matrix
        SparseMatrix D = getSecondOrderFiniteDifferenceMatrix(bspline);

        // Left-hand side matrix
        A = Bt*W*B + alpha*D.transpose()*D;

        // Compute right-hand side matrices
        b = Bt*W*b;
    }

    DenseMatrix x;

    int numEquations = A.rows();
    int maxNumEquations = 100;
    bool solveAsDense = (numEquations < maxNumEquations);

    if (!solveAsDense)
    {
        #ifndef NDEBUG
        std::cout << "BSpline::Builder::computeBSplineCoefficients: Computing B-spline control points using sparse solver." << std::endl;
        #endif // NDEBUG

        SparseLU<DenseMatrix> s;
        //bool successfulSolve = (s.solve(A,Bx,Cx) && s.solve(A,By,Cy));

        solveAsDense = !s.solve(A, b, x);
    }

    if (solveAsDense)
    {
        #ifndef NDEBUG
        std::cout << "BSpline::Builder::computeBSplineCoefficients: Computing B-spline control points using dense solver." << std::endl;
        #endif // NDEBUG

        DenseMatrix Ad = A.toDense();
        DenseQR<DenseMatrix> s;
        // DenseSVD<DenseVector> s;
        //bool successfulSolve = (s.solve(Ad,Bx,Cx) && s.solve(Ad,By,Cy));
        if (!s.solve(Ad, b, x))
        {
            throw Exception("BSpline::Builder::computeBSplineCoefficients: Failed to solve for B-spline coefficients.");
        }
    }

    return x;
}

SparseMatrix BSpline::Builder::computeBasisFunctionMatrix(const BSpline &bspline, const DataTable &data) const
{
    unsigned int numSamples = data.getNumSamples();

    // TODO: Reserve nnz per row (degree+1)
    //int nnzPrCol = bspline.basis.numSupported();

    SparseMatrix A(numSamples, bspline.getNumBasisFunctions());
    //A.reserve(DenseVector::Constant(numSamples, nnzPrCol)); // TODO: should reserve nnz per row!

    int i = 0;
    for (auto it = data.cbegin(); it != data.cend(); ++it, ++i)
    {
        DenseVector xi = stdToEigVec(it->getX());
        SparseVector basis_values = bspline.evalBasis(xi);
        for (SparseVector::InnerIterator it2(basis_values); it2; ++it2)
            A.insert(i, it2.index()) = it2.value();
    }

    A.makeCompressed();

    return A;
}

DenseMatrix BSpline::Builder::stackSamplePointValues(const DataTable &data) const
{
    DenseMatrix B = DenseMatrix::Zero(data.getNumSamples(), data.getDimY());

    int i = 0;
    for (auto it = data.cbegin(); it != data.cend(); ++it, ++i)
    {
        auto y = it->getY();
        for (unsigned int j = 0; j < data.getDimY(); ++j)
            B(i, j) = y.at(j);
    }
    return B;
}

/*
* Function for generating second order finite-difference matrix, which is used for penalizing the
* (approximate) second derivative in control point calculation for P-splines.
*/
SparseMatrix BSpline::Builder::getSecondOrderFiniteDifferenceMatrix(const BSpline &bspline) const
{
    unsigned int numVariables = bspline.getDimX();

    // Number of (total) basis functions - defines the number of columns in D
    unsigned int numCols = bspline.getNumBasisFunctions();
    std::vector<unsigned int> numBasisFunctions = bspline.getNumBasisFunctionsPerVariable();

    // Number of basis functions (and coefficients) in each variable
    std::vector<unsigned int> dims;
    for (unsigned int i = 0; i < numVariables; i++)
        dims.push_back(numBasisFunctions.at(i));

    std::reverse(dims.begin(), dims.end());

    for (unsigned int i=0; i < numVariables; ++i)
        if (numBasisFunctions.at(i) < 3)
            throw Exception("BSpline::Builder::getSecondOrderDifferenceMatrix: Need at least three coefficients/basis function per variable.");

    // Number of rows in D and in each block
    int numRows = 0;
    std::vector< int > numBlkRows;
    for (unsigned int i = 0; i < numVariables; i++)
    {
        int prod = 1;
        for (unsigned int j = 0; j < numVariables; j++)
        {
            if (i == j)
                prod *= (dims[j] - 2);
            else
                prod *= dims[j];
        }
        numRows += prod;
        numBlkRows.push_back(prod);
    }

    // Resize and initialize D
    SparseMatrix D(numRows, numCols);
    D.reserve(DenseVector::Constant(numCols, 2*numVariables));   // D has no more than two elems per col per dim

    int i = 0;                                          // Row index
    // Loop though each dimension (each dimension has its own block)
    for (unsigned int d = 0; d < numVariables; d++)
    {
        // Calculate left and right products
        int leftProd = 1;
        int rightProd = 1;
        for (unsigned int k = 0; k < d; k++)
        {
            leftProd *= dims[k];
        }
        for (unsigned int k = d+1; k < numVariables; k++)
        {
            rightProd *= dims[k];
        }

        // Loop through subblocks on the block diagonal
        for (int j = 0; j < rightProd; j++)
        {
            // Start column of current subblock
            int blkBaseCol = j*leftProd*dims[d];
            // Block rows [I -2I I] of subblock
            for (unsigned int l = 0; l < (dims[d] - 2); l++)
            {
                // Special case for first dimension
                if (d == 0)
                {
                    int k = j*leftProd*dims[d] + l;
                    D.insert(i,k) = 1;
                    k += leftProd;
                    D.insert(i,k) = -2;
                    k += leftProd;
                    D.insert(i,k) = 1;
                    i++;
                }
                else
                {
                    // Loop for identity matrix
                    for (int n = 0; n < leftProd; n++)
                    {
                        int k = blkBaseCol + l*leftProd + n;
                        D.insert(i,k) = 1;
                        k += leftProd;
                        D.insert(i,k) = -2;
                        k += leftProd;
                        D.insert(i,k) = 1;
                        i++;
                    }
                }
            }
        }
    }

    D.makeCompressed();

    return D;
}

// Compute all knot vectors from sample data
std::vector<std::vector<double> > BSpline::Builder::computeKnotVectors(const DataTable &data) const
{
    if (_dim_x != _degrees.size())
        throw Exception("BSpline::Builder::computeKnotVectors: Inconsistent sizes on input vectors.");

    std::vector<std::vector<double>> grid = data.getTableX();

    std::vector<std::vector<double>> knotVectors;

    for (unsigned int i = 0; i < _dim_x; ++i)
    {
        // Compute knot vector
        auto knotVec = computeKnotVector(grid.at(i), _degrees.at(i), _numBasisFunctions.at(i));

        knotVectors.push_back(knotVec);
    }

    return knotVectors;
}

// Compute a single knot vector from sample grid and degree
std::vector<double> BSpline::Builder::computeKnotVector(const std::vector<double> &values,
                                                        unsigned int degree,
                                                        unsigned int numBasisFunctions) const
{
    switch (_knotSpacing)
    {
        case KnotSpacing::AS_SAMPLED:
            return knotVectorMovingAverage(values, degree);
        case KnotSpacing::EQUIDISTANT:
            return knotVectorEquidistant(values, degree, numBasisFunctions);
        case KnotSpacing::EXPERIMENTAL:
            return knotVectorEquidistantNotClamped(values, degree, numBasisFunctions);
        default:
            return knotVectorMovingAverage(values, degree);
    }
}

} // namespace SPLINTER