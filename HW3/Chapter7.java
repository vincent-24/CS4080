/*  CHAPTER 7 QUESTION 2

    case PLUS:
        if (left instanceof String || right instanceof String) 
            return stringify(left) + stringify(right);

        if (left instanceof Double && right instanceof Double) 
            return (double)left + (double)right;

*/

/* CHAPTER 7 QUESTION 3

    case SLASH: 
        checkNumberOperands(expr.operator, left, right); 

        if ((double)right == 0) 
            throw new RuntimeError(expr.operator, "Divide by zero error."); 

        return (double)left / (double)right;

*/