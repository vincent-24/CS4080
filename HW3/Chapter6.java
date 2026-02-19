/*  CHAPTER 6 QUESTION 1

    if (!check(RIGHT_PAREN)) {
        do {
            if (arguments.size() >= 8)
                error(peek(), "Can't have more than 8 arguments.");

        arguments.add(equality()); // <-- was expression().
        } while (match(COMMA));
    }

    private Expr expression() {
        return comma();
    }

    private Expr comma() {
        Expr expr = equality();

        while (match(COMMA)) {
            Token operator = previous();
            Expr right = equality();
            expr = new Expr.Binary(expr, operator, right);
        }

        return expr;
    }

*/

/*  CHAPTER 6 QUESTION 2

	private Expr expression() {
      	return conditional();
    }

   	private Expr conditional() {
      	Expr expr = equality();

      	if (match(QUESTION)) {
        		Expr thenBranch = expression();
        		consume(COLON, "Expect ':' after conditional.");
        		Expr elseBranch = conditional();
      		expr = new Expr.Conditional(expr, thenBranch, elseBranch);
      	}

      	return expr;
    }

*/

/*  CHAPTER 6 QUESTION 3

    private Expr primary() {
        if (match(FALSE)) return new Expr.Literal(false);
        if (match(TRUE)) return new Expr.Literal(true);
        if (match(NIL)) return new Expr.Literal(null);

        if (match(NUMBER, STRING)) {
            return new Expr.Literal(previous().literal);
        }

        if (match(LEFT_PAREN)) {
            Expr expr = expression();
            consume(RIGHT_PAREN, "Expect ')' after expression.");
            return new Expr.Grouping(expr);
        }

        if (match(BANG_EQUAL, EQUAL_EQUAL)) {
            return errorProduction("Missing left operand.", this::comparison);
        }

        if (match(GREATER, GREATER_EQUAL, LESS, LESS_EQUAL)) {
            return errorProduction("Missing left operand.", this::term);
        }

        if (match(PLUS)) {
            return errorProduction("Missing left operand.", this::factor);
        }

        if (match(SLASH, STAR)) {
            return errorProduction("Missing left operand.", this::unary);
        }

        throw error(peek(), "Expect expression.");
    }

    private Expr errorProduction(String message, Supplier<Expr> rule) {
        error(previous(), message);
        rule.get(); 
        return new Expr.Literal(null); 
    }

*/