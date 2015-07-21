/*++
Copyright (c) 2015 Microsoft Corporation

Module Name:

    qepr.h

Abstract:

    EPR symbol elimination routine

Author:

    Nikolaj Bjorner (nbjorner) 2015-7-16

Revision History:


Notes:

 

Extraction of cores and assumptions:
------------------------------------

          | Core              | Assumptions
---------------------------------------------
E P, x, y | Done              | None
A Q       | Learn conflict    | atomic predicates, disequalities over x,y,z forced by evaluation of Q
E z       | Add core to l0    | atomic predicates, Graphs for Q, P  
A 0       | Add core to l1    | atomic predicates 


--*/

#include "qepr.h"

#include "smt_kernel.h"
#include "qe_mbp.h"
#include "smt_params.h"
#include "ast_util.h"
#include "quant_hoist.h"
#include "ast_pp.h" 
#include "model_v2_pp.h"
#include "filter_model_converter.h"
#include "array_decl_plugin.h"
#include "expr_abstract.h"
#include "qsat.h"
#include "obj_pair_set.h"

namespace qe {


    class qepr : public tactic  {

        struct stats {
            unsigned m_num_rounds;        
            stats() { reset(); }
            void reset() { memset(this, 0, sizeof(*this)); }
        };

        typedef obj_map<func_decl, ptr_vector<app> > pred2occs;

        ast_manager&               m;
        params_ref                 m_params;
        pred_abs                   m_pred_abs;
        smt_params                 m_smtp;
        model_ref                  m_model;
        volatile bool              m_cancel;
        statistics                 m_st;
        qe::mbp                    m_mbp;
        smt::kernel                m_fa;
        smt::kernel                m_ex;
        unsigned                   m_level;
        stats                      m_stats;
        expr_ref_vector            m_answer;
        app_ref_vector             m_free_vars;   // free variables
        app_ref_vector             m_bound_vars;  // universally bound variables
        func_decl_ref_vector       m_free_preds;  // predicates to project
        func_decl_ref_vector       m_bound_preds; // predicates to project
        pred2occs                  m_free_pred_occs;
        pred2occs                  m_bound_pred_occs;


        lbool check_sat() {
            while (true) {
                ++m_stats.m_num_rounds;
                check_cancel();
                expr_ref_vector asms(m);
                get_assumptions(asms);
                smt::kernel& k = get_kernel(m_level);
                lbool res = k.check(asms);
                switch (res) {
                case l_true:
                    k.get_model(m_model);
                    TRACE("qe", k.display(tout); display(tout << "\n", *m_model.get()); display(tout, asms); );
                    push();
                    break;
                case l_false:
                    if (m_level == 0) {
                        return l_false;
                    }
                    project();
                    break;
                case l_undef:
                    return res;
                }
            }
            return l_undef;
        }

        void check_cancel() {
            if (m_cancel) {
                throw tactic_exception(TACTIC_CANCELED_MSG);
            }
        }

        void pop(unsigned num_scopes) {
            m_model.reset();
            m_pred_abs.pop(num_scopes);
            SASSERT(num_scopes <= m_level);
            m_level -= num_scopes;
        }

        void push() {
            m_pred_abs.push();
            ++m_level;
        }

        void project() {
            expr_ref_vector core(m);
            expr_ref fml(m);
            get_core(core, m_level);
            SASSERT(m_level > 0);
            TRACE("qe", display(tout); display(tout << "core\n", core););
            if (m_level == 1) {
                fml = negate_core(core);
                m_ex.assert_expr(fml);
                m_answer.push_back(fml);
                pop(1);
            }
            else if (!m_model.get()) {
                // we can at most reach level 3,
                // after which the state is 
                // backjumped to level 1 or 0
                // level 0: project is not called. 
                // level 1: previous case applies
                UNREACHABLE();  
                pop(1);
            }
            else {
                SASSERT(m_level <= 3);
                // m_level = 2, 3:
                // create negated core.
                
                fml = negate_core(core);
                m_ex.assert_expr(fml);
                m_fa.assert_expr(fml);
                m_level -= 2;
            }
        }
    
        void get_assumptions(expr_ref_vector& asms) {
            switch (m_level) {
            case 0:
                asms.reset();
                break;
            case 1: 
                ensure_disequalities();
                m_pred_abs.get_assumptions(m_model.get(), asms);
                break;            
            case 2:
                m_pred_abs.get_assumptions(m_model.get(), asms);
                // get assumptions should ensure that there are no predicates 
                // of the form Q(z), P(z) so the graph of P, Q
                // at these arguments is encoded as constraints.
                extract_function_graphs(asms);
                break;            
            case 3:
                // all atoms can be used without change.
                m_pred_abs.get_assumptions(m_model.get(), asms);
                break;
            }             
            TRACE("qe", tout << asms << "\n";);
        }

        void ensure_disequalities() {
            // Q(x), !Q(z) then we need x != z as decision of player 0
            pred2occs pos, neg;
            expr_ref_vector diseqs(m), defs(m);
            expr_ref val(m);                
            collect_pos_neg(m_bound_pred_occs, pos, neg);
            extract_disequalities(pos, neg, diseqs);
            for (unsigned i = 0; i < diseqs.size(); ++i) {
                m_pred_abs.abstract_atoms(diseqs[i].get(), defs);
            }
            assert_defs(defs);
        }

        void assert_defs(expr_ref_vector const& defs) {
            expr_ref val(m);
            expr* a, *b;
            for (unsigned j = 0; j < defs.size(); ++j) {
                VERIFY(m.is_eq(defs[j], a, b));
                VERIFY(m_model->eval(b, val));
                m_model->register_decl(to_app(a)->get_decl(), val);
                m_fa.assert_expr(defs[j]);
                m_ex.assert_expr(defs[j]);
            }            
        }

        void extract_disequalities(pred2occs& pos, pred2occs& neg, expr_ref_vector& diseqs) {
            model& mdl = *m_model.get();
            pred2occs::iterator it = pos.begin(), end = pos.end();
            expr_ref val1(m), val2(m);
            obj_pair_set<expr,expr> known_diseq;
            for (; it != end; ++it) {
                func_decl* f = it->m_key;
                pred2occs::obj_map_entry* e = neg.find_core(f);
                if (!e) {
                    continue;
                }
                for (unsigned i = 0; i < it->m_value.size(); ++i) {
                    app* p = it->m_value[i];
                    for (unsigned j = 0; j < e->get_data().m_value.size(); ++j) {
                        app* n = e->get_data().m_value[j];
                        // recycle already applied disequalities.
                        bool skip_me = false;
                        for (unsigned k = 0; !skip_me && !known_diseq.empty() && k < p->get_num_args(); ++k) {
                            skip_me = 
                                known_diseq.contains(p->get_arg(k), n->get_arg(k)) ||
                                known_diseq.contains(n->get_arg(k), p->get_arg(k));
                        }
                        if (skip_me) {
                            continue;
                        }
                        for (unsigned k = 0; k < p->get_num_args(); ++k) {
                            VERIFY(mdl.eval(p->get_arg(k), val1));
                            VERIFY(mdl.eval(n->get_arg(k), val2));
                            if (val1 != val2) {
                                diseqs.push_back(m.mk_not(m.mk_eq(p->get_arg(k), n->get_arg(k))));
                                known_diseq.insert(p->get_arg(k), n->get_arg(k));
                                skip_me = true;
                                break;
                            }
                        }
                        SASSERT(skip_me);
                    }
                }
            }
        }
        
        void extract_function_graphs(expr_ref_vector& asms) {
            pred2occs::iterator it1 = m_bound_pred_occs.begin(), end1 = m_bound_pred_occs.end();
            for (; it1 != end1; ++it1) {
                extract_function_graph(it1->m_key, it1->m_value, asms);
            }
            pred2occs::iterator it2 = m_free_pred_occs.begin(), end2 = m_free_pred_occs.end();
            for (; it2 != end2; ++it2) {
                extract_function_graph(it2->m_key, it2->m_value, asms);
            }
        }

        void extract_function_graph(func_decl* p, ptr_vector<app> const& occs, expr_ref_vector& asms) {

            pred2occs pos, neg;
            collect_pos_neg(occs, pos, neg);

            //
            // p(x), p(y), !p(z), !p(u)
            // p = \ w . (w = x or w = y) & w != z & w != u
            // p = \ w . true if there are no negations
            // p = \ w . false if there are no non-negations
            //
            SASSERT(pos.contains(p) || neg.contains(p));
            if (!neg.contains(p)) {
                ptr_vector<app> const& poss = pos.find(p);
                for (unsigned i = 0; i < poss.size(); ++i) {
                    push_asms(asms, poss[i]);
                }
                return;
            }
            if (!pos.contains(p)) {
                ptr_vector<app> const& negs = neg.find(p);
                for (unsigned i = 0; i < negs.size(); ++i) {
                    expr_ref fml(m.mk_not(negs[i]), m);
                    push_asms(asms, fml);
                }
                return;
            }
            ptr_vector<app> const& poss = pos.find(p);
            ptr_vector<app> const& negs = neg.find(p);
            for (unsigned i = 0; i < poss.size(); ++i) {
                max_level l = m_pred_abs.compute_level(poss[i]);
                if (l.max() == 2) {
                    push_asms(asms, mk_graph(poss[i], poss, negs));
                }
            }
            for (unsigned i = 0; i < negs.size(); ++i) {
                max_level l = m_pred_abs.compute_level(negs[i]);
                if (l.max() == 2) {
                    push_asms(asms, mk_graph(negs[i], poss, negs));
                }
            }            
        }

        void push_asms(expr_ref_vector& asms, expr* a) {
            expr_ref_vector defs(m);
            app_ref lit = m_pred_abs.mk_assumption_literal(a, defs);
            assert_defs(defs);
            asms.push_back(lit);
        }

        expr_ref mk_graph(app* p, ptr_vector<app> const& pos, ptr_vector<app> const& neg) {
            expr_ref fml(m);
            expr_ref_vector fmls(m);
            for (unsigned i = 0; i < pos.size(); ++i) {
                fmls.push_back(eq_args(p, pos[i]));
            }
            fml = mk_or(fmls);
            fmls.reset();
            fmls.push_back(fml);
            for (unsigned i = 0; i < neg.size(); ++i) {
                fmls.push_back(m.mk_not(eq_args(p, neg[i])));
            }      
            fml = m.mk_iff(p, mk_and(fmls));
            return fml;
        }

        expr_ref eq_args(app* p, app* q) {
            expr_ref_vector eqs(m);
            for (unsigned i = 0; i < p->get_num_args(); ++i) {
                expr* a = p->get_arg(i);
                expr* b = q->get_arg(i);
                if (a != b) {
                    eqs.push_back(m.mk_eq(a, b));
                }
            }
            return mk_and(eqs);
        }

        void add_predicate(pred2occs& map, expr* _p) {
            app* p = to_app(_p);
            map.insert_if_not_there2(p->get_decl(), ptr_vector<app>())->get_data().m_value.push_back(p);
        }

        void get_core(expr_ref_vector& core, unsigned level) {
            smt::kernel& k = get_kernel(level);
            unsigned sz = k.get_unsat_core_size();
            core.reset();
            for (unsigned i = 0; i < sz; ++i) {
                core.push_back(k.get_unsat_core_expr(i));
            }
            m_pred_abs.mk_concrete(core);
            TRACE("qe", tout << "core: " << core << "\n"; k.display(tout); tout << "\n";);
        }

    
        void collect_pos_neg(pred2occs const& preds, pred2occs& pos, pred2occs& neg) {
            expr_ref val(m);
            pred2occs::iterator it = preds.begin(), end = preds.end();
            for (; it != end; ++it) {
                collect_pos_neg(it->m_value, pos, neg);
            }
        }
        
        void collect_pos_neg(ptr_vector<app> const& occs, pred2occs& pos, pred2occs& neg) {
            expr_ref val(m);
            for (unsigned i = 0; i < occs.size(); ++i) {
                VERIFY(m_model->eval(occs[i], val));
                if (m.is_true(val)) {
                    add_predicate(pos, occs[i]);
                }
                else {
                    add_predicate(neg, occs[i]);
                }
            }
        }            
        

    

        /**
           \brief Create fresh equality atoms for each equality that holds in current model among vars.
         */
        void extract_equalities(app_ref_vector const& vars, expr_ref_vector& defs) {
        }
     
        smt::kernel& get_kernel(unsigned l) {
            return ((l % 2) == 0)?m_ex:m_fa;
        }

        smt::kernel const& get_kernel(unsigned l) const {
            return ((l % 2) == 0)?m_ex:m_fa;
        }

        expr_ref negate_core(expr_ref_vector& core) {
            expr_ref fml(m);
            app_ref_vector bound(m_bound_vars);
            m_mbp.solve(*m_model.get(), bound, core);
            fml = ::push_not(::mk_and(core));
            fml = mk_forall(m, bound.size(), bound.c_ptr(), fml);
            return fml;
        }

        void hoist(expr_ref& fml) {
            m_free_vars.reset();
            m_bound_vars.reset();
            quantifier_hoister hoist(m);
            m_pred_abs.get_free_vars(fml, m_free_vars);
            hoist.pull_quantifier(true, fml, m_bound_vars);
            set_level(0, m_free_vars);
            set_level(2, m_bound_vars);
            collect_predicates(fml);
        }

        void set_level(unsigned l, app_ref_vector const& vars) {
            max_level lvl;
            lvl.m_ex = l;
            for (unsigned i = 0; i < vars.size(); ++i) {
                m_pred_abs.set_expr_level(vars[i], lvl);
            }
        }

        /**
           \brief Collect predicates to eliminate.
         */
        void collect_predicates(expr* fml) {
            m_free_preds.reset();
            m_bound_preds.reset();
            m_free_pred_occs.reset();
            m_bound_pred_occs.reset();
            ast_fast_mark1 mark;
            ptr_vector<expr> todo;
            todo.push_back(fml);
            while (!todo.empty()) {
                expr* e = todo.back();
                if (mark.is_marked(e) || is_var(e)) continue;
                mark.mark(e);
                if (is_quantifier(e)) {
                    todo.push_back(to_quantifier(e)->get_expr());
                    continue;
                }
                app* a = to_app(e);
                func_decl* d = a->get_decl();
                if (!mark.is_marked(d) && is_bound_predicate(d)) {
                    m_bound_preds.push_back(d);
                    m_bound_pred_occs.insert_if_not_there2(d, ptr_vector<app>())->get_data().m_value.push_back(a);
                }
                if (!mark.is_marked(d) && is_free_predicate(d)) {
                    m_free_preds.push_back(d);
                    m_free_pred_occs.insert_if_not_there2(d, ptr_vector<app>())->get_data().m_value.push_back(a);

                }
                mark.mark(d);
                for (unsigned i = 0; i < a->get_num_args(); ++i) {
                    todo.push_back(a->get_arg(i));
                }                    
            }            
        }

        bool is_bound_predicate(expr* e) {
            return 
                is_app(e) && 
                is_bound_predicate(to_app(e)->get_decl());
        }

        bool is_bound_predicate(func_decl* d) {
            return 
                is_predicate(m, d) && 
                d->get_family_id() == null_family_id &&
                to_eliminate(d->get_name());
        }

        bool is_free_predicate(func_decl* d) {
            return 
                is_predicate(m, d) && 
                d->get_family_id() == null_family_id && 
                !to_eliminate(d->get_name());
        }
        
        /*
          \brief at this point use and trust underscores to identify predicates to eliminate.
         */
        bool to_eliminate(symbol const& s) {
            return !s.is_numerical() && s.bare_str() && s.bare_str()[0] == '_';
        }

        void display(std::ostream& out) const {
            out << "Level:       " << m_level << "\n";
            out << "Free vars:   " << m_free_vars << "\n";
            out << "Free preds:  " << m_free_preds << "\n";
            out << "Bound vars:  " << m_bound_vars << "\n";
            out << "Bound preds: " << m_bound_preds << "\n";
            m_pred_abs.display(out);
        }

        void display(std::ostream& out, model& model) const {
            display(out);
            model_v2_pp(out, model);
        }

        void display(std::ostream& out, expr_ref_vector const& asms) const {
            m_pred_abs.display(out, asms);
        }

    public:

        qepr(ast_manager& m, params_ref const& p): 
            m(m),            
            m_params(p),
            m_pred_abs(m),
            m_cancel(false),
            m_mbp(m),
            m_fa(m, m_smtp),
            m_ex(m, m_smtp),
            m_answer(m),
            m_free_preds(m),
            m_bound_preds(m),
            m_free_vars(m),
            m_bound_vars(m)
        {
            m_smtp.m_model = true;
            m_smtp.m_relevancy_lvl = 0;
        }

        virtual ~qepr() {
            reset();
        }

        virtual void set_cancel(bool f) {
            m_fa.set_cancel(f);        
            m_ex.set_cancel(f);        
            m_cancel = f;
        }
        
        virtual tactic* translate(ast_manager& m) {
            return alloc(qepr, m, m_params);
        }
        
        virtual void set_progress_callback(progress_callback* cb) {
        }

        void set_logic(symbol const& l) {
        }

        void cleanup() {
            reset();
            set_cancel(false);
        }

        void collect_statistics(statistics & st) const {
            st.copy(m_st);
            m_pred_abs.collect_statistics(st);
            st.update("qsat num rounds", m_stats.m_num_rounds); 
        }
        
        void reset_statistics() {
            m_stats.reset();
            m_fa.reset_statistics();
            m_ex.reset_statistics();        
        }

        virtual void operator()(
            /* in */  goal_ref const & in, 
            /* out */ goal_ref_buffer & result, 
            /* out */ model_converter_ref & mc, 
            /* out */ proof_converter_ref & pc,
            /* out */ expr_dependency_ref & core) {
            tactic_report report("qsat-tactic", *in);
            ptr_vector<expr> fmls;
            expr_ref_vector defs(m);
            expr_ref fml(m);
            mc = 0; pc = 0; core = 0;
            in->get_formulas(fmls);
            fml = mk_and(m, fmls.size(), fmls.c_ptr());
            hoist(fml);
            m_pred_abs.abstract_atoms(fml, defs);
            fml = m_pred_abs.mk_abstract(fml);
            m_ex.assert_expr(mk_and(defs));
            m_fa.assert_expr(mk_and(defs));
            m_fa.assert_expr(fml);
            fml = m.mk_not(fml);
            m_ex.assert_expr(fml);

            
            TRACE("qe", m_fa.display(tout); tout << "\n"; display(tout););
            
            lbool is_sat = check_sat();
            
            switch (is_sat) {
            case l_false:
                in->reset();
                in->inc_depth();
                fml = mk_and(m_answer);
                in->assert_expr(fml);
                result.push_back(in.get());
                break;
            case l_true:
                UNREACHABLE();                
            case l_undef:
                result.push_back(in.get());
                std::string s = m_ex.last_failure_as_string() + m_fa.last_failure_as_string();
                throw tactic_exception(s.c_str()); 
            }
        }

        virtual void reset() {
            m_pred_abs.collect_statistics(m_st);        
            m_fa.collect_statistics(m_st);
            m_ex.collect_statistics(m_st);

            m_level = 0;
            m_answer.reset();
            m_free_vars.reset();
            m_bound_vars.reset();
            m_free_preds.reset();
            m_bound_preds.reset();
            m_free_pred_occs.reset();
            m_bound_pred_occs.reset();
            m_model = 0;
            m_pred_abs.reset();
            m_st.reset();
            m_fa.reset();
            m_ex.reset();        
            m_cancel = false;            
        }
    };
}

tactic * mk_epr_qe_tactic(ast_manager& m, params_ref const& p) {
    qe::qepr* qs = alloc(qe::qepr, m, p);
    return qs;
}

