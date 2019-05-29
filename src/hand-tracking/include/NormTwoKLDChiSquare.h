/*
 * Copyright (C) 2016-2019 Istituto Italiano di Tecnologia (IIT)
 *
 * This software may be modified and distributed under the terms of the
 * BSD 3-Clause license. See the accompanying LICENSE file for details.
 */

#ifndef NORMTWOKLDCHISQUARE_H
#define NORMTWOKLDCHISQUARE_H

#include <BayesFilters/LikelihoodModel.h>

#include <memory>


class NormTwoKLDChiSquare : public bfl::LikelihoodModel
{
public:
    NormTwoKLDChiSquare(const double likelihood_gain, const std::size_t vector_size) noexcept;

    ~NormTwoKLDChiSquare() noexcept;

    std::pair<bool, Eigen::VectorXd> likelihood(const bfl::MeasurementModel& measurement_model, const Eigen::Ref<const Eigen::MatrixXd>& pred_states) override;

private:
    struct ImplData;

    std::unique_ptr<ImplData> pImpl_;
};

#endif /* NORMTWOKLDCHISQAUARE_H */
