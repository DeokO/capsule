#include "capsule.h"

Capsule::Capsule(model_settings* model_set, Data* dataset) {
    settings = model_set;
    data = dataset;

    // phi: entity conerns
    printf("\tinitializing entity concerns (phi)\n");
    phi_k = fmat(settings->k, data->entity_count());
    logphi_k = fmat(settings->k, data->entity_count());
    a_phi_k = fmat(settings->k, data->entity_count());
    b_phi_k = fmat(settings->k, data->entity_count());

    phi_d = fmat(data->date_count(), data->entity_count());
    logphi_d = fmat(data->date_count(), data->entity_count());
    a_phi_d = fmat(data->date_count(), data->entity_count());
    b_phi_d = fmat(data->date_count(), data->entity_count());

    // theta: topics
    printf("\tinitializing topics (theta)\n");
    theta = fmat(settings->k, data->term_count());
    logtheta = fmat(settings->k, data->term_count());
    a_theta = fmat(settings->k, data->term_count());
    b_theta = fmat(settings->k, data->term_count());

    // pi: event descriptions
    printf("\tinitializing event descriptions (pi)\n");
    pi = fmat(data->date_count(), data->term_count());
    logpi = fmat(data->date_count(), data->term_count());
    a_pi = fmat(data->date_count(), data->term_count());
    b_pi = fmat(data->date_count(), data->term_count());

    // decay function, for ease
    decay = fmat(data->date_count(), data->date_count());
    logdecay = fmat(data->date_count(), data->date_count());

    // keep track of old a parameters for SVI
    a_phi_k_old = fmat(settings->k, data->entity_count());
    a_phi_k_old.fill(settings->a_phi);
    a_phi_d_old = fmat(data->date_count(), data->entity_count());
    a_phi_d_old.fill(settings->a_phi);
    a_theta_old = fmat(settings->k, data->term_count());
    a_theta_old.fill(settings->a_theta);
    a_pi_old = fmat(data->date_count(), data->term_count());
    a_pi_old.fill(settings->a_pi);

    printf("\tsetting random seed\n");
    rand_gen = gsl_rng_alloc(gsl_rng_taus);
    gsl_rng_set(rand_gen, (long) settings->seed); // init the seed

    initialize_parameters();

    scale = settings->svi ? float(data->train_doc_count()) / float(settings->sample_size) : 1;
}

void Capsule::learn() {
    double old_likelihood, delta_likelihood, likelihood = -1e10;
    int likelihood_decreasing_count = 0;
    time_t start_time, end_time;

    int iteration = 0;
    char iter_as_str[4];
    bool converged = false;
    bool on_final_pass = false;

    while (!converged) {
        time(&start_time);
        iteration++;
        printf("iteration %d\n", iteration);

        reset_helper_params();

        set<int> terms;
        set<int> entities;
        set<int> dates;
        int doc, term, count, entity, date;
        for (int i = 0; i < settings->sample_size; i++) {
            if (settings->svi) {
                doc = gsl_rng_uniform_int(rand_gen, data->train_doc_count());
            } else {
                doc = i;
            }

            entities.insert(data->get_entity(doc));
            date = data->get_date(doc);
            for (int d = max(0, date - settings->event_dur); d <= date; d++) {
                dates.insert(d);
            }
            // look at all the document's terms
            for (int j = 0; j < data->term_count(doc); j++) {
                term = data->get_term(doc, j);
                terms.insert(term);

                count = data->get_term_count(doc, j);
                update_shape(doc, term, count);
            }
        }

        set<int>::iterator it, it2;
        if (!settings->event_only) {
            b_theta.each_col() += sum(phi_k, 1);
            for (it = terms.begin(); it != terms.end(); it++) {
                term = *it;
                //if (iter_count_term[term] == 0)
                //    iter_count_term[term] = 0;
                iter_count_term[term]++;
                update_theta(term);
            }
            //for (int k = 0; k < settings->k; k++) {
            //    logtheta.row(k) -= log(accu(theta.row(k)));
            //    theta.row(k) /= accu(theta.row(k));
           // }

            b_phi_k.each_col() += sum(theta, 1);
        }

        if (!settings->entity_only) {
            for (it = dates.begin(); it != dates.end(); it++) {
                date = *it;
                iter_count_date[date]++;
                for (int d = date; d < min(date + settings->event_dur, data->date_count()); d++) {
                    for (it2 = entities.begin(); it2 != entities.end(); it2++) {
                        entity = *it2;
                        b_pi.row(date) += f(d, date) * phi_d(date, entity) * data->doc_count(entity, d);
                        //printf("d %d, date %d, f %f\n", d, date, f(d, date));
                    }
                }
                printf("updating event description %d\n", date);
                update_pi(date);
            }

            for (it = dates.begin(); it != dates.end(); it++) {
                date = *it;
                for (int d = date; d < min(date + settings->event_dur, data->date_count()); d++) {
                    for (it2 = entities.begin(); it2 != entities.end(); it2++) {
                        entity = *it2;
                        b_phi_d(date, entity) += f(d, date) * accu(pi.col(d)) * data->doc_count(entity, d);
                    }
                }
            }
        }

        for (it = entities.begin(); it != entities.end(); it++) {
            entity = *it;
            iter_count_entity[entity]++;
            update_phi(entity);
        }

        // check for convergence
        if (on_final_pass) {
            printf("Final pass complete\n");
            converged = true;

            old_likelihood = likelihood;
            likelihood = get_ave_log_likelihood();
            delta_likelihood = abs((old_likelihood - likelihood) /
                old_likelihood);
            log_convergence(iteration, likelihood, delta_likelihood);
        } else if (iteration >= settings->max_iter) {
            printf("Reached maximum number of iterations.\n");
            converged = true;

            old_likelihood = likelihood;
            likelihood = get_ave_log_likelihood();
            delta_likelihood = abs((old_likelihood - likelihood) /
                old_likelihood);
            log_convergence(iteration, likelihood, delta_likelihood);
        } else if (iteration % settings->conv_freq == 0) {
            old_likelihood = likelihood;
            likelihood = get_ave_log_likelihood();

            if (likelihood < old_likelihood)
                likelihood_decreasing_count += 1;
            else
                likelihood_decreasing_count = 0;
            delta_likelihood = abs((old_likelihood - likelihood) /
                old_likelihood);
            log_convergence(iteration, likelihood, delta_likelihood);
            if (settings->verbose) {
                printf("delta: %f\n", delta_likelihood);
                printf("old:   %f\n", old_likelihood);
                printf("new:   %f\n", likelihood);
            }
            if (iteration >= settings->min_iter &&
                delta_likelihood < settings->likelihood_delta) {
                printf("Model converged.\n");
                converged = true;
            } else if (iteration >= settings->min_iter &&
                likelihood_decreasing_count >= 2) {
                printf("Likelihood decreasing.\n");
                converged = true;
            }
        }

        // save intermediate results
        if (!converged && settings->save_freq > 0 &&
            iteration % settings->save_freq == 0) {
            printf(" saving\n");
            sprintf(iter_as_str, "%04d", iteration);
            save_parameters(iter_as_str);
        }

        // intermediate evaluation
        if (!converged && settings->eval_freq > 0 &&
            iteration % settings->eval_freq == 0) {
            sprintf(iter_as_str, "%04d", iteration);
            evaluate(iter_as_str);
        }

        time(&end_time);
        log_time(iteration, difftime(end_time, start_time));

        if (converged && !on_final_pass && settings->final_pass) {
            printf("final pass on all users.\n");
            on_final_pass = true;
            converged = false;

            // we need to modify some settings for the final pass
            // things should look exactly like batch for all users
            settings->set_stochastic_inference(false);
            settings->set_sample_size(data->train_doc_count());
            scale = 1;
        }
    }

    save_parameters("final");
}

double Capsule::predict(int doc, int term) {
    double prediction = 0;
    int entity = data->get_entity(doc);
    if (!settings->event_only) {
        prediction += accu(phi_k.col(entity) % theta.col(term));
    }

    if (!settings->entity_only) {
        int date = data->get_date(doc);
        for (int d = max(0, date - settings->event_dur); d <= date; d++)
            prediction += f(date, d) * phi_d(d,entity) * pi(d,term);
    }

    return prediction;
}

// helper function to sort predictions properly
bool prediction_compare(const pair<pair<double,int>, int>& itemA,
    const pair<pair<double, int>, int>& itemB) {
    // if the two values are equal, sort by popularity!
    if (itemA.first.first == itemB.first.first) {
        if (itemA.first.second == itemB.first.second)
            return itemA.second < itemB.second;
        return itemA.first.second > itemB.first.second;
    }
    return itemA.first.first > itemB.first.first;
}

void Capsule::evaluate() {
    evaluate("final", true);
}

void Capsule::evaluate(string label) {
    //evaluate(label, false);
    evaluate(label, true);
}

void Capsule::evaluate(string label, bool write_rankings) {
    time_t start_time, end_time;
    time(&start_time);

    eval(this, &Model::predict, settings->outdir, data, false, settings->seed,
        settings->verbose, label, write_rankings);

    time(&end_time);
    log_time(-1, difftime(end_time, start_time));
}



/* PRIVATE */

void Capsule::initialize_parameters() {
    if (!settings->event_only) {
        phi_k.fill(settings->a_phi / settings->b_phi);
        logphi_k.fill(log(settings->a_phi / settings->b_phi));

        // topics
        //theta.fill(settings->a_theta / settings->b_theta);
        //logtheta.fill(gsl_sf_psi(settings->a_theta) -
        //    log(settings->b_theta));
        for (int k = 0; k < settings->k; k++) {
            for (int v = 0; v < data->term_count(); v++) {
                theta(k, v) = (settings->a_theta +
                    gsl_rng_uniform_pos(rand_gen))
                    / (settings->b_theta);
                logtheta(k, v) = log(theta(k, v));
            }
            logtheta.row(k) -= log(accu(theta.row(k)));
            theta.row(k) /= accu(theta.row(k));
        }
    }

    //printf("init?\n");
    if (!settings->entity_only) {
        phi_d.fill(settings->a_phi / settings->b_phi);
        logphi_d.fill(log(settings->a_phi / settings->b_phi));

        // event descriptions
        pi.fill(settings->a_pi / settings->b_pi);
        logpi.fill(log(settings->a_pi / settings->b_pi));

        // log f function
        for (int i = 0; i < data->date_count(); i++) {
            for (int j = 0; j < data->date_count(); j++) {
                // recall: i=docdate, j=eventdate
                decay(i,j) = f(i,j);
                logdecay(i,j) = log(decay(i,j));
            }
        }
    }
}

void Capsule::reset_helper_params() {
    a_phi_k.fill(settings->a_phi);
    b_phi_k.fill(settings->b_phi);
    a_phi_d.fill(settings->a_phi);
    b_phi_d.fill(settings->b_phi);
    a_theta.fill(settings->a_theta);
    b_theta.fill(settings->b_theta);
    a_pi.fill(settings->a_pi);
    b_pi.fill(settings->b_pi);
}

void Capsule::save_parameters(string label) {
    FILE* file;

    if (!settings->event_only) {
        int k;

        // write out phi
        file = fopen((settings->outdir+"/phi-k-"+label+".dat").c_str(), "w");
        for (int entity = 0; entity < data->entity_count(); entity++) {
            fprintf(file, "%d", entity);
            for (k = 0; k < settings->k; k++)
                fprintf(file, "\t%e", phi_k(k, entity));
            fprintf(file, "\n");
        }
        fclose(file);

        // write out theta
        file = fopen((settings->outdir+"/theta-"+label+".dat").c_str(), "w");
        for (int term = 0; term < data->term_count(); term++) {
            fprintf(file, "%d", term);
            for (k = 0; k < settings->k; k++)
                fprintf(file, "\t%e", theta(k, term));
            fprintf(file, "\n");
        }
        fclose(file);
    }

    if (!settings->entity_only) {
        int t;

        // write out phi (dates)
        file = fopen((settings->outdir+"/phi-d-"+label+".dat").c_str(), "w");
        for (int entity = 0; entity < data->entity_count(); entity++) {
            fprintf(file, "%d", entity);
            for (int date = 0; date < data->date_count(); date++)
                fprintf(file, "\t%e", phi_d(date, entity));
            fprintf(file, "\n");
        }
        fclose(file);

        // write out pi
        file = fopen((settings->outdir+"/pi-"+label+".dat").c_str(), "w");
        for (int date = 0; date < data->date_count(); date++) {
            fprintf(file, "%d", date);
            for (t = 0; t < data->term_count(); t++)
                fprintf(file, "\t%e", pi(date, t));
            fprintf(file, "\n");
        }
        fclose(file);
    }
}

void Capsule::update_shape(int doc, int term, int count) {
    int entity = data->get_entity(doc);
    int date = data->get_date(doc);
    //printf("\tauthor %d and date %d\n", entity, date);

    double omega_sum = 0;

    fvec omega_entity, omega_event;
    if (!settings->event_only) {
        omega_entity = exp(logphi_k.col(entity) + logtheta.col(term));
        omega_sum += accu(omega_entity);
    }

    if (!settings->entity_only) {
        omega_event = fvec(data->date_count());
        for (int d = max(0, date - settings->event_dur + 1); d <= date; d++) {
            omega_event(d) = exp(logphi_d(d,entity) + logpi(d,term) + logdecay(date,d));
            omega_sum += omega_event(d);
        }
    }

    if (omega_sum == 0)
        return;

    if (!settings->event_only) {
        omega_entity /= omega_sum * count;
        a_phi_k.col(entity) += omega_entity * scale;
        a_theta.col(term) += omega_entity * scale;
    }

    if (!settings->entity_only) {
        omega_event /= omega_sum * count;
        for (int d = max(0,date - settings->event_dur + 1); d <= date; d++) {
            a_phi_d(d,entity) += omega_event[d] * scale;
            a_pi(d,term) += omega_event[d] * scale;
        }
    }
}

void Capsule::update_phi(int entity) {
    if (settings->svi) {
        double rho = pow(iter_count_entity[entity] + settings->delay,
            -1 * settings->forget);
        if (!settings->event_only) {
            a_phi_k.col(entity) = (1 - rho) * a_phi_k_old.col(entity) + rho * a_phi_k.col(entity);
            a_phi_k_old.col(entity) = a_phi_k.col(entity);
        }
        if (!settings->entity_only) {
            a_phi_d.col(entity) = (1 - rho) * a_phi_d_old.col(entity) + rho * a_phi_d.col(entity);
            a_phi_d_old.col(entity) = a_phi_d.col(entity);
        }
    }
    if (!settings->event_only) {
        for (int k = 0; k < settings->k; k++) {
            phi_k(k, entity)  = a_phi_k(k, entity) / b_phi_k(k, entity);
            logphi_k(k, entity) = gsl_sf_psi(a_phi_k(k, entity));
        }
        logphi_k.col(entity) = logphi_k.col(entity) - log(b_phi_k.col(entity));
    }
    if (!settings->entity_only) {
        for (int d = 0; d < data->date_count(); d++) {
            phi_d(d, entity)  = a_phi_d(d, entity) / b_phi_d(d, entity);
            logphi_d(d, entity) = gsl_sf_psi(a_phi_d(d, entity));
        }
        logphi_d.col(entity) = logphi_d.col(entity) - log(b_phi_d.col(entity));
    }
}

void Capsule::update_theta(int term) {
    if (settings->svi) {
        double rho = pow(iter_count_term[term] + settings->delay,
            -1 * settings->forget);
        a_theta.col(term) = (1 - rho) * a_theta_old.col(term) + rho * a_theta.col(term);
        a_theta_old.col(term) = a_theta.col(term);
    }
    //if (term == 3) {
    //    for (int k = 0; k < settings->k; k++)
    //        printf("term %d, k %d\ta: %f, b: %f => %f = %f\n", term, k, a_theta(k,term), b_theta(k,term), a_theta(k, term) / b_theta(k, term), theta(k, term));
    //}
    for (int k = 0; k < settings->k; k++) {
        theta(k, term)  = a_theta(k, term) / b_theta(k, term);
        logtheta(k, term) = gsl_sf_psi(a_theta(k, term));
    }
    logtheta.col(term) = logtheta.col(term) - log(b_theta.col(term));
}

void Capsule::update_pi(int date) {
    if (settings->svi) {
        double rho = pow(iter_count_date[date] + settings->delay,
            -1 * settings->forget);
        a_pi.row(date) = (1 - rho) * a_pi_old.row(date) + rho * a_pi.row(date);
        a_pi_old.row(date) = a_pi.row(date);
    }
    //if (date == 55) {
    //    for (int v = 0; v < data->term_count(); v++)
    //        printf("date %d, term %d\ta: %f, b: %f => %f = %f\n", date, v, a_pi(date,v), b_pi(date,v), a_pi(date,v)/ b_pi(date,v), pi(date,v));
    //}
    for (int v = 0; v < data->term_count(); v++) {
        pi(date, v) = a_pi(date, v) / b_pi(date, v);
        //if (v == 11)
        //    printf("word 11: %f / %f\n", a_pi(date, v), b_pi(date, v));
        logpi(date, v) = gsl_sf_psi(a_pi(date, v));
    }
    logpi.row(date) = logpi.row(date) - log(b_pi.row(date));

    logpi.row(date) -= log(accu(pi.row(date)));
    pi.row(date) /= accu(pi.row(date));
}

double Capsule::point_likelihood(double pred, int truth) {
    //return log(pred) * truth - log(factorial(truth)) - pred; (est)
    return log(pred) * truth - pred;
}

double Capsule::get_ave_log_likelihood() {//TODO: rename (it's not ave)
    double prediction, likelihood = 0;
    int doc, term, count;
    for (int i = 0; i < data->num_validation(); i++) {
        doc = data->get_validation_doc(i);
        term = data->get_validation_term(i);
        count = data->get_validation_count(i);

        prediction = predict(doc, term);

        likelihood += point_likelihood(prediction, count);
        //double ll = point_likelihood(prediction, count);
        //likelihood += ll;
        //printf("\t%f\t[%d]\t=> + %f\n", prediction, count, ll);
    }

    printf("likelihood %f\n", likelihood);

    return likelihood;// / data->num_validation();
}

double Capsule::p_gamma(fmat x, fmat a, fmat b) {
    mat lga = zeros<mat>(a.n_rows, a.n_cols);
    for (uint r =0; r < a.n_rows; r++) {
        for (uint c=0; c < a.n_cols; c++) {
            lga(r,c) = lgamma(a(r,c));
        }
    }
    return accu((a-ones(a.n_rows, a.n_cols)) % log(x) - b % x - a % log(b) - lga);
}

double Capsule::p_gamma(fmat x, double a, double b) {
    return accu((a-1) * log(x) - b * x - a * log(b) - lgamma(a));
}

double Capsule::p_gamma(fvec x, fvec a, fvec b) {
    vec lga = zeros<vec>(a.n_elem);
    for (uint i = 0; i < a.n_elem; i++){
        lga(i) = lgamma(a(i));
    }
    return accu((a-ones(a.n_elem)) % log(x) - b % x - a % log(b) - lga);
}

double Capsule::p_gamma(fvec x, double a, double b) {
    return accu((a-1) * log(x) - b * x - a * log(b) - lgamma(a));
}

double Capsule::elbo_extra() {
    double rv, rvtotal = 0;

    if (!settings->entity_only) {
        rv = p_gamma(pi, a_pi, b_pi);
        //printf("q pi (event descr): %f\n", rv);
        printf("%f\t", rv);
        rvtotal += rv;

        rv = p_gamma(phi_d, a_phi_d, b_phi_d);
        //printf("q phi-d (ent evt): %f\n", rv);
        printf("%f\t", rv);
        rvtotal += rv;
    }

    if (!settings->event_only) {
        rv = p_gamma(phi_k, a_phi_k, b_phi_k);
        //printf("q phi-k (entity con): %f\n", rv);
        printf("%f\t", rv);
        rvtotal += rv;

        rv = p_gamma(theta, a_theta, b_theta);
        //printf("q theta (topics):   %f\n", rv);
        printf("%f\t", rv);
        rvtotal += rv;
    }

    if (!settings->entity_only) {
        rv = p_gamma(pi, settings->a_pi, settings->b_pi);
        //printf("p pi (event descr): %f\n", rv);
        printf("%f\t", rv);
        rvtotal += rv;

        rv = p_gamma(phi_d, settings->a_phi, settings->a_phi);
        //printf("p phi_d (entevt): %f\n", rv);
        printf("%f\t", rv);
        rvtotal += rv;
    }

    if (!settings->event_only) {
        rv = p_gamma(phi_k, settings->a_phi, settings->b_phi);
        //printf("p phi_k (entity con): %f\n", rv);
        printf("%f\t", rv);
        rvtotal += rv;

        rv = p_gamma(theta, settings->a_theta, settings->b_theta);
        //printf("p theta (topics):   %f\n", rv);
        printf("%f\n", rv);
        rvtotal += rv;
    }

    return rvtotal;
}

void Capsule::log_convergence(int iteration, double ave_ll, double delta_ll) {
    FILE* file = fopen((settings->outdir+"/log_likelihood.dat").c_str(), "a");
    if (settings->event_only)
        printf("q evt desc\t\tq evtent\t\tp evt desc\tp evtent\n");
    else if (settings->entity_only)
        printf("q entity\tq topics\t\tp entity\tp topics\n");
    else
        printf("q evt desc\t\tq evtent\t\tq entity\tq topics\tp evt desc\tp evtent\t\tp entity\tp topics\n");
    double ee = elbo_extra();
    printf("ll %f\tq/p %f\n", ave_ll, ee);
    fprintf(file, "%d\t%f\t%f\t%f\n", iteration, ave_ll+ee, ave_ll, delta_ll);
    printf("ll: %f\n", ave_ll);
    printf("total: %f\n", ee + ave_ll);

    //fprintf(file, "%d\t%f\t%f\t%f\n", iteration, ave_ll+elbo_extra(), ave_ll, delta_ll);
    fclose(file);
}

void Capsule::log_time(int iteration, double duration) {
    FILE* file = fopen((settings->outdir+"/time_log.dat").c_str(), "a");
    fprintf(file, "%d\t%.f\n", iteration, duration);
    fclose(file);
}

void Capsule::log_user(FILE* file, int user, int heldout, double rmse, double mae,
    double rank, int first, double crr, double ncrr, double ndcg) {
    fprintf(file, "%d\t%f\t%f\t%f\t%d\t%f\t%f\t%f\n", user,
        rmse, mae, rank, first, crr, ncrr, ndcg);
}

double Capsule::f(int doc_date, int event_date) {
    // this can be confusing: the document of int
    if (event_date > doc_date || event_date <= (doc_date - settings->event_dur))
        return 0;
    return (1.0-(0.0+doc_date-event_date)/settings->event_dur);
}

double Capsule::get_event_strength(int date) {
    double total = 0;
    for (int entity = 0; entity < data->entity_count(); entity++)
        total += phi_d(date, entity);
    return total;
}
