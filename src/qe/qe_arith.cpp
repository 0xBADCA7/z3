/*++
Copyright (c) 2015 Microsoft Corporation

Module Name:

    qe_arith.cpp

Abstract:

    Simple projection function for real arithmetic based on Loos-W.

Author:

    Nikolaj Bjorner (nbjorner) 2013-09-12

Revision History:



--*/

#include "qe_arith.h"
#include "qe_util.h"
#include "arith_decl_plugin.h"
#include "ast_pp.h"
#include "th_rewriter.h"
#include "expr_functors.h"
#include "model_v2_pp.h"

namespace qe {

    bool is_divides(arith_util& a, expr* e1, expr* e2, rational& k, expr_ref& p) {  
        expr* t1, *t2;
        ast_manager& m = a.get_manager();
        if (a.is_mod(e2, t1, t2) && 
            a.is_numeral(e1, k) && 
            k.is_zero() &&
            a.is_numeral(t2, k)) {
            p = t1;
            return true;
        }
        return false;
    }

    bool is_divides(arith_util& a, expr* e, rational& k, expr_ref& t) {
        expr* e1, *e2;
        if (!a.get_manager().is_eq(e, e1, e2)) {
            return false;
        }
        return is_divides(a, e1, e2, k, t) || is_divides(a, e2, e1, k, t);
    }
    
    class arith_project_util {
        ast_manager&      m;
        arith_util        a;
        th_rewriter       m_rw;
        expr_ref_vector   m_ineq_terms;
        vector<rational>  m_ineq_coeffs;
        svector<bool>     m_ineq_strict;
        expr_ref_vector   m_div_terms;
        vector<rational>  m_div_divisors, m_div_coeffs;
        expr_ref_vector   m_new_lits;
        rational m_delta, m_u;
        scoped_ptr<contains_app> m_var;

        struct cant_project {};

        sort* var_sort() const { return m.get_sort(m_var->x()); }

        bool is_int() const { return a.is_int(m_var->x()); }

        void display(std::ostream& out) const {
            for (unsigned i = 0; i < num_ineqs(); ++i) {
                display_ineq(out, i);
            }
            for (unsigned i = 0; i < num_divs(); ++i) {
                display_div(out, i);
            }            
        }

        void is_linear(model& model, rational const& mul, expr* t, rational& c, expr_ref_vector& ts) {
            expr* t1, *t2;
            rational mul1;
            expr_ref val(m);
            if (t == m_var->x()) {
                c += mul;
            }
            else if (a.is_mul(t, t1, t2) && a.is_numeral(t1, mul1)) {
                is_linear(model, mul* mul1, t2, c, ts);
            }
            else if (a.is_mul(t, t1, t2) && a.is_numeral(t2, mul1)) {
                is_linear(model, mul* mul1, t1, c, ts);
            }
            else if (a.is_add(t)) {
                app* ap = to_app(t);
                for (unsigned i = 0; i < ap->get_num_args(); ++i) {
                    is_linear(model, mul, ap->get_arg(i), c, ts);
                }
            }
            else if (a.is_sub(t, t1, t2)) {
                is_linear(model, mul,  t1, c, ts);
                is_linear(model, -mul, t2, c, ts);
            }
            else if (a.is_uminus(t, t1)) {
                is_linear(model, -mul, t1, c, ts);
            }
            else if (a.is_numeral(t, mul1)) {
                ts.push_back(mk_num(mul*mul1));
            }            
            else if (extract_mod(model, t, val)) {
                ts.push_back(mk_mul(mul, val));
            }
            else if ((*m_var)(t)) {
                TRACE("qe", tout << "can't project:" << mk_pp(t, m) << "\n";);
                throw cant_project();
            }
            else {
                ts.push_back(mk_mul(mul, t));
            }
        }


        bool is_linear(model& model, expr* lit, rational& c, expr_ref& t, bool& is_strict, bool& is_eq) {
            expr* e1, *e2;
            c.reset();
            expr_ref_vector ts(m);            
            bool is_not = m.is_not(lit, lit);
            rational mul(1);
            is_eq = false;
            if (is_not) {
                mul.neg();
            }
            SASSERT(!m.is_not(lit));
            if (a.is_le(lit, e1, e2) || a.is_ge(lit, e2, e1)) {
                is_linear(model,  mul, e1, c, ts);
                is_linear(model, -mul, e2, c, ts);
                is_strict = is_not;
            }
            else if (a.is_lt(lit, e1, e2) || a.is_gt(lit, e2, e1)) {
                is_linear(model,  mul, e1, c, ts);
                is_linear(model, -mul, e2, c, ts);
                is_strict = !is_not;
            }
            else if (m.is_eq(lit, e1, e2) && !is_not && is_arith(e1)) {
                is_linear(model,  mul, e1, c, ts);
                is_linear(model, -mul, e2, c, ts);
                is_strict = false;
                is_eq = true;
            }  
            else if (m.is_eq(lit, e1, e2) && is_not && is_arith(e1)) {
                expr_ref val1(m), val2(m);
                rational r1, r2;
                VERIFY(model.eval(e1, val1) && a.is_numeral(val1, r1));
                VERIFY(model.eval(e2, val2) && a.is_numeral(val2, r2));
                SASSERT(r1 != r2);
                if (r1 < r2) {
                    std::swap(e1, e2);
                }                
                is_strict = true;
                is_linear(model,  mul, e1, c, ts);
                is_linear(model, -mul, e2, c, ts);                               
            }
            else {
                TRACE("qe", tout << "can't project:" << mk_pp(lit, m) << "\n";);
                throw cant_project();
            }
            if (is_strict && is_int()) {
                ts.push_back(mk_num(1));
                is_strict = false;
            }
            t = add(ts);
            if (is_eq && c.is_neg()) {
                t = a.mk_uminus(t);
                c.neg();
            }
            return true;
        }

        bool is_arith(expr* e) {
            return a.is_int(e) || a.is_real(e);
        }

        expr_ref add(expr_ref_vector const& ts) {
            if (ts.empty()) {
                return mk_num(0);
            }
            else {
                return expr_ref(a.mk_add(ts.size(), ts.c_ptr()), m);
            }
        }


        // e is of the form  (ax + t) mod k
        bool is_mod(model& model, expr* e, rational& k, expr_ref& t, rational& c) {
            expr* t1, *t2;
            expr_ref_vector ts(m);
            if (a.is_mod(e, t1, t2) &&
                a.is_numeral(t2, k) &&
                (*m_var)(t1)) {
                c.reset();
                rational mul(1);
                is_linear(model, mul, t1, c, ts);
                t = add(ts);
                return true;
            }
            return false;
        }

        bool extract_mod(model& model, expr* e, expr_ref& val) {
            rational c, k;
            expr_ref t(m);
            if (is_mod(model, e, k, t, c)) {                
                VERIFY (model.eval(e, val));
                SASSERT (a.is_numeral(val));
                TRACE("qe", tout << "extract: " << mk_pp(e, m) << " evals: " << val << " c: " << c << " t: " << t << "\n";);
                if (!c.is_zero()) {
                    t = a.mk_sub(t, val);
                    m_div_terms.push_back(t);
                    m_div_divisors.push_back(k);
                    m_div_coeffs.push_back(c);
                }
                else {
                    t = m.mk_eq(a.mk_mod(t, mk_num(k)), val);
                    add_lit(model, m_new_lits, t);
                }
                return true;
            }
            return false;
        }

        bool lit_is_true(model& model, expr* e) {
            expr_ref val(m);
            VERIFY(model.eval(e, val));
            CTRACE("qe", !m.is_true(val), tout << "eval: " << mk_pp(e, m) << " " << val << "\n";);
            return m.is_true(val);
        }

        expr_ref mk_num(unsigned n) {
            rational r(n);
            return mk_num(r);
        }

        expr_ref mk_num(rational const& r) const {
            return expr_ref(a.mk_numeral(r, var_sort()), m);
        }

        expr_ref mk_divides(rational const& k, expr* t) {            
            return expr_ref(m.mk_eq(a.mk_mod(t, mk_num(abs(k))), mk_num(0)), m);
        }

        void reset() {
            m_ineq_terms.reset();
            m_ineq_coeffs.reset();
            m_ineq_strict.reset();
            m_div_terms.reset();
            m_div_coeffs.reset();
            m_div_divisors.reset();
            m_delta = rational(1);
            m_u     = rational(0);
            m_new_lits.reset();
        }

        expr* ineq_term(unsigned i) const { return m_ineq_terms[i]; }
        rational const& ineq_coeff(unsigned i) const { return m_ineq_coeffs[i]; }
        bool ineq_strict(unsigned i) const { return m_ineq_strict[i]; }
        app_ref mk_ineq_pred(unsigned i) { 
            app_ref result(m);
            result = a.mk_add(a.mk_mul(mk_num(ineq_coeff(i)), m_var->x()), ineq_term(i));
            if (ineq_strict(i)) {
                result = a.mk_lt(result, mk_num(0));
            }
            else {
                result = a.mk_le(result, mk_num(0));
            }
            return result;
        }
        void display_ineq(std::ostream& out, unsigned i) const {
            out << mk_pp(ineq_term(i), m) << " " << ineq_coeff(i) << "*" << mk_pp(m_var->x(), m);
            if (ineq_strict(i)) out << " < 0";
            else out << " <= 0";
            out << "\n";
        }
        unsigned num_ineqs() const { return m_ineq_terms.size(); }
        expr* div_term(unsigned i) const { return m_div_terms[i]; }
        rational const& div_coeff(unsigned i) const { return m_div_coeffs[i]; }
        rational const& div_divisor(unsigned i) const { return m_div_divisors[i]; }
        void display_div(std::ostream& out, unsigned i) const {
            out << div_divisor(i) << " | ( " << mk_pp(div_term(i), m) << " " << div_coeff(i) << "*" 
                << mk_pp(m_var->x(), m) << ")\n";
        }
        unsigned num_divs() const { return m_div_terms.size(); }

        void project(model& model, expr_ref_vector& lits) {
            TRACE("qe", 
                  tout << "project: " << mk_pp(m_var->x(), m) << "\n";
                  for (unsigned i = 0; i < lits.size(); ++i) {
                      tout << mk_pp(lits[i].get(), m) << "\n";
                  }
                  model_v2_pp(tout, model); );
                        
            unsigned num_pos = 0, num_neg = 0, eq_index = 0;
            reset();
            bool found_eq = false;
            for (unsigned i = 0; i < lits.size(); ++i) {
                rational c(0);
                expr_ref t(m);
                bool is_strict, is_eq;
                expr* e = lits[i].get();
                if (!(*m_var)(e)) {
                    m_new_lits.push_back(e);                    
                }
                else if (is_linear(model, e, c, t, is_strict, is_eq)) {
                    if (c.is_zero()) {
                        if (is_eq) {
                            t = a.mk_eq(t, mk_num(0));
                        }
                        else if (is_strict) {
                            t = a.mk_lt(t, mk_num(0));
                        }
                        else {
                            t = a.mk_le(t, mk_num(0));
                        }
                        add_lit(model, m_new_lits, t);
                    }
                    else {
                        m_ineq_coeffs.push_back(c);
                        m_ineq_terms.push_back(t);
                        m_ineq_strict.push_back(is_strict);
                        if (is_eq) {
                            found_eq = true;
                            eq_index = m_ineq_coeffs.size()-1;
                        }
                        else if (c.is_pos()) {
                            ++num_pos;
                        }
                        else {
                            ++num_neg;
                        }            
                    }        
                }
                else {
                    TRACE("qe", tout << "can't project:" << mk_pp(e, m) << "\n";);
                    throw cant_project();
                }
            }
            TRACE("qe", display(tout << mk_pp(m_var->x(), m) << " "););
            lits.reset();
            lits.append(m_new_lits);
            if (found_eq) {
                apply_equality(model, eq_index, lits);
                return;
            }
            if (num_divs() == 0 && (num_pos == 0 || num_neg == 0)) {
                return;
            }
            if (num_divs() > 0) {
                apply_divides(model, lits);
                TRACE("qe", display(tout << "after division " << mk_pp(m_var->x(), m) << "\n"););
            }
            if (num_pos == 0 || num_neg == 0) {
                return;
            }
            bool use_pos = num_pos < num_neg;
            unsigned max_t = find_max(model, use_pos);

            for (unsigned i = 0; i < m_ineq_terms.size(); ++i) {
                expr_ref t(m);
                if (i != max_t) {
                    if (ineq_coeff(i).is_pos() == use_pos) {
                        t = mk_le(i, max_t);
                        add_lit(model, lits, t);
                    }
                    else {
                        mk_lt(model, lits, i, max_t);
                    }
                }
            }
            TRACE("qe", for (unsigned i = 0; i < lits.size(); ++i) {
                    tout << mk_pp(lits[i].get(), m) << "\n";
                });
        }

        unsigned find_max(model& mdl, bool do_pos) {
            unsigned result;
            bool new_max = true;
            rational max_r, r;
            expr_ref val(m);
            bool is_int = a.is_int(m_var->x());
            for (unsigned i = 0; i < m_ineq_terms.size(); ++i) {
                rational const& ac = m_ineq_coeffs[i];
                SASSERT(!is_int || !ineq_strict(i));

                //
                // ac*x + t < 0
                // ac > 0: x + max { t/ac | ac > 0 } < 0   <=> x < - max { t/ac | ac > 0 }
                // ac < 0: x + t/ac > 0 <=> x > max { - t/ac | ac < 0 } = max { t/|ac| | ac < 0 } 
                //
                if (ac.is_pos() == do_pos) {
                    VERIFY(mdl.eval(ineq_term(i), val));
                    VERIFY(a.is_numeral(val, r));
                    r /= abs(ac);
                    new_max =
                        new_max || 
                        (r > max_r) || 
                        (r == max_r && ineq_strict(i)) ||
                        (r == max_r && is_int && ac.is_one());
                    TRACE("qe", tout << "max: "  << mk_pp(ineq_term(i), m) << "/" << abs(ac) << " := " << r << " " 
                          << (new_max?"":"not ") << "new max\n";);
                    if (new_max) { 
                        result = i;
                        max_r  = r;
                    }
                    new_max = false;
                }
            }
            SASSERT(!new_max);
            return result;
        }

        // ax + t <= 0
        // bx + s <= 0
        // a and b have different signs.
        // Infer: a|b|x + |b|t + |a|bx + |a|s <= 0
        // e.g.   |b|t + |a|s <= 0
        void mk_lt(model& model, expr_ref_vector& lits, unsigned i, unsigned j) {
            rational const& ac = ineq_coeff(i);
            rational const& bc = ineq_coeff(j);
            SASSERT(ac.is_pos() != bc.is_pos());
            SASSERT(ac.is_neg() != bc.is_neg());
            if (is_int() && !abs(ac).is_one() && !abs(bc).is_one()) {
                return mk_int_lt(model, lits, i, j);
            }
            expr* t = ineq_term(i);
            expr* s = ineq_term(j);
            expr_ref bt = mk_mul(abs(bc), t);
            expr_ref as = mk_mul(abs(ac), s);
            expr_ref ts = mk_add(bt, as);
            expr_ref  z = mk_num(0);
            expr_ref  fml(m);
            if (ineq_strict(i) || ineq_strict(j)) {
                fml = a.mk_lt(ts, z);
            }
            else {
                fml = a.mk_le(ts, z);
            }
            add_lit(model, lits, fml);
        }

        void mk_int_lt(model& model, expr_ref_vector& lits, unsigned i, unsigned j) {
            expr* t = ineq_term(i);
            expr* s = ineq_term(j);
            rational const& ac = ineq_coeff(i);
            rational const& bc = ineq_coeff(j);
            expr_ref tmp(m);
            SASSERT(!ineq_strict(i) && !ineq_strict(j));
            rational abs_a = abs(ac);
            rational abs_b = abs(bc);
            expr_ref as(mk_mul(abs_a, s), m);
            expr_ref bt(mk_mul(abs_b, t), m);
            
            rational slack = (abs_a - rational(1))*(abs_b-rational(1));
            rational sval, tval;
            VERIFY (model.eval(ineq_term(i), tmp) && a.is_numeral(tmp, tval));
            VERIFY (model.eval(ineq_term(j), tmp) && a.is_numeral(tmp, sval));
            bool use_case1 = ac*sval + bc*tval + slack <= rational(0);
            if (use_case1) {
                std::cout << "slack " << slack << "\n";
                expr_ref_vector ts(m);
                ts.push_back(as);
                ts.push_back(bt);
                ts.push_back(mk_num(-slack));
                tmp = a.mk_le(add(ts), mk_num(0));
                add_lit(model, lits, tmp);
                return;
            }

            rational a1 = ac, b1 = bc;
            if (abs_a < abs_b) {
                std::swap(abs_a, abs_b);
                std::swap(a1, b1);
                std::swap(s, t);
                std::swap(as, bt);
                std::swap(sval, tval);
            }
            SASSERT(abs_a >= abs_b);                
            
            // create finite disjunction for |b|.                                
            //    exists x, z in [0 .. |b|-2] . b*x + s + z = 0 && ax + t <= 0 && bx + s <= 0
            // <=> 
            //    exists x, z in [0 .. |b|-2] . b*x = -z - s && ax + t <= 0 && bx + s <= 0
            // <=>
            //    exists x, z in [0 .. |b|-2] . b*x = -z - s && a|b|x + |b|t <= 0 && bx + s <= 0
            // <=>
            //    exists x, z in [0 .. |b|-2] . b*x = -z - s && a|b|x + |b|t <= 0 && -z - s + s <= 0
            // <=>
            //    exists x, z in [0 .. |b|-2] . b*x = -z - s && a|b|x + |b|t <= 0 && -z <= 0
            // <=>
            //    exists x, z in [0 .. |b|-2] . b*x = -z - s && a|b|x + |b|t <= 0
            // <=>
            //    exists x, z in [0 .. |b|-2] . b*x = -z - s && a*n_sign(b)(s + z) + |b|t <= 0
            // <=>
            //    exists z in [0 .. |b|-2] . |b| | (z + s) && a*n_sign(b)(s + z) + |b|t <= 0
            //
              
            rational z = mod(sval, abs_b);
            if (!z.is_zero()) z = abs_b - z;
            expr_ref s_plus_z(mk_add(z, s), m);
            
            TRACE("qe", display_ineq(tout, i); display_ineq(tout, j););
            tmp = mk_divides(abs_b, s_plus_z);
            add_lit(model, lits, tmp);
            tmp = a.mk_le(mk_add(mk_mul(a1*n_sign(b1), s_plus_z),
                                 mk_mul(abs_b, t)), mk_num(0));
            add_lit(model, lits, tmp);
        }

        rational n_sign(rational const& b) {
            return rational(b.is_pos()?-1:1);
        }

        // ax + t <= 0
        // bx + s <= 0
        // a and b have same signs.
        // encode:
        // t/|a| <= s/|b|
        // e.g.   |b|t <= |a|s
        expr_ref mk_le(unsigned i, unsigned j) {
            rational const& ac = ineq_coeff(i);
            rational const& bc = ineq_coeff(j);
            SASSERT(ac.is_pos() == bc.is_pos());
            SASSERT(ac.is_neg() == bc.is_neg());
            expr* t = ineq_term(i);
            expr* s = ineq_term(j);
            expr_ref bt = mk_mul(abs(bc), t);
            expr_ref as = mk_mul(abs(ac), s);
            if (ineq_strict(i) && !ineq_strict(j)) {
                return expr_ref(a.mk_lt(bt, as), m);
            }
            else {
                return expr_ref(a.mk_le(bt, as), m);
            }
        }

        expr_ref mk_add(expr* t1, expr* t2) {
            return expr_ref(a.mk_add(t1, t2), m);
        }
        expr_ref mk_add(rational const& r, expr* e) {
            if (r.is_zero()) return expr_ref(e, m);
            return mk_add(mk_num(r), e);
        }
                                  
        expr_ref mk_mul(rational const& r, expr* t) {
            if (r.is_one()) return expr_ref(t, m);
            return expr_ref(a.mk_mul(mk_num(r), t), m);
        }

        void add_lit(model& model, expr_ref_vector& lits, expr* e) { 
            expr_ref orig(e, m), result(m);
            m_rw(orig, result);            
            TRACE("qe", tout << mk_pp(orig, m) << " -> " << result << "\n";); 
            SASSERT(lit_is_true(model, orig));
            if (!m.is_true(result)) {
                lits.push_back(result);
            }
        }


        // 3x + t = 0 & 7 | (c*x + s) & ax <= u 
        // 3 | -t  & 21 | (-ct + 3s) & a-t <= 3u

        void apply_equality(model& model, unsigned eq_index, expr_ref_vector& lits) {
            rational c = ineq_coeff(eq_index);
            expr* t = ineq_term(eq_index);
            SASSERT(c.is_pos());
            if (is_int()) {
                add_lit(model, lits, mk_divides(c, ineq_term(eq_index)));
            }
            
            for (unsigned i = 0; i < num_divs(); ++i) {
                add_lit(model, lits, mk_divides(c*div_divisor(i), 
                                                a.mk_sub(mk_mul(c, div_term(i)), mk_mul(div_coeff(i), t))));
            }
            for (unsigned i = 0; i < num_ineqs(); ++i) {
                if (eq_index != i) {
                    expr_ref lhs(m);
                    lhs = a.mk_sub(mk_mul(c, ineq_term(i)), mk_mul(ineq_coeff(i), t));
                    if (ineq_strict(i)) {
                        add_lit(model, lits, a.mk_lt(lhs, mk_num(0)));
                    }
                    else {
                        add_lit(model, lits, a.mk_le(lhs, mk_num(0)));
                    }
                }
            }
        }

        // 
        // compute D and u.
        //
        // D = lcm(d1, d2)
        // u = eval(x) mod D
        // 
        //   d1 | (a1x + t1) & d2 | (a2x + t2)
        // = 
        //   D | (D/d1)(a1x + t1) & D | (D/d2)(a2x + t2)
        // =
        //   D | D1(a1*u + t1) & D | D2(a2*u + t2) & x = D*x' + u & 0 <= u < D
        // =
        //   D | D1(a1*u + t1) & D | D2(a2*u + t2) & x = D*x' + u & 0 <= u < D
        // 
        // x := D*x' + u
        // 
        void apply_divides(model& model, expr_ref_vector& lits) {
            SASSERT(m_delta.is_one());
            unsigned n = num_divs();
            if (n == 0) {
                return;
            }
            for (unsigned i = 0; i < n; ++i) {
                m_delta = lcm(m_delta, div_divisor(i));
            }
            expr_ref val(m);
            rational r;
            VERIFY (model.eval(m_var->x(), val) && a.is_numeral(val, r));
            m_u = mod(r, m_delta);
            SASSERT(m_u < m_delta && rational(0) <= m_u);
            for (unsigned i = 0; i < n; ++i) {
                add_lit(model, lits, mk_divides(div_divisor(i), 
                                         a.mk_add(mk_num(div_coeff(i) * m_u), div_term(i))));
            }
            //
            // update inequalities such that u is added to t and
            // D is multiplied to coefficient of x.
            // the interpretation of the new version of x is (x-u)/D
            //
            for (unsigned i = 0; i < num_ineqs(); ++i) {
                if (!m_u.is_zero()) {
                    m_ineq_terms[i] = a.mk_sub(ineq_term(i), mk_num(m_u));
                }
                m_ineq_coeffs[i] *= m_delta;
            }
            r = (r - m_u) / m_delta;
            SASSERT(r.is_int());
            val = a.mk_numeral(r, true);
            model.register_decl(m_var->x()->get_decl(), val);
            TRACE("qe", model_v2_pp(tout, model););
        }

    public:
        arith_project_util(ast_manager& m): 
            m(m), a(m), m_rw(m), m_ineq_terms(m), m_div_terms(m), m_new_lits(m) {
            params_ref params;
            params.set_bool("gcd_rouding", true);
            m_rw.updt_params(params);
        }

        expr_ref operator()(model& model, app_ref_vector& vars, expr_ref_vector const& lits) {
            app_ref_vector new_vars(m);
            expr_ref_vector result(lits);
            for (unsigned i = 0; i < vars.size(); ++i) {
                app* v = vars[i].get();
                if (a.is_real(v) || a.is_int(v)) {
                    m_var = alloc(contains_app, m, v);
                    try {
                        project(model, result);
                        TRACE("qe", tout << "projected: " << mk_pp(v, m) << "\n";
                              for (unsigned i = 0; i < result.size(); ++i) {
                                  tout << mk_pp(result[i].get(), m) << "\n";
                              });
                    }
                    catch (cant_project) {
                        TRACE("qe", tout << "can't project:" << mk_pp(v, m) << "\n";);
                        new_vars.push_back(v);
                    }
                }
                else {
                    new_vars.push_back(v);
                }
            }
            vars.reset();
            vars.append(new_vars);
            return qe::mk_and(result);
        }  
    };

    expr_ref arith_project(model& model, app_ref_vector& vars, expr_ref_vector const& lits) {
        ast_manager& m = vars.get_manager();
        arith_project_util ap(m);
        return ap(model, vars, lits);
    }

    expr_ref arith_project(model& model, app_ref_vector& vars, expr* fml) {
        ast_manager& m = vars.get_manager();
        arith_project_util ap(m);
        expr_ref_vector lits(m);
        qe::flatten_and(fml, lits);
        return ap(model, vars, lits);
    }

}
