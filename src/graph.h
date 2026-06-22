#ifndef GRAPH_H
#define GRAPH_H

#include "ast.h"

/* Lazy build-graph backend: lowers a parsed program into an in-memory graph
 * of build nodes (glob / morphism / pipe / command / record / tuple / guard
 * / run ...). Nothing is expanded or executed here -- the terminal run()
 * node is built but never spawned (that is the deferred stage). */
void graph_run_program(ast *prog);

#endif /* GRAPH_H */
