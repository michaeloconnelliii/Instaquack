/*
 * string_parser.h
 *
 *  Created on: Nov 8, 2020
 *      Author: gguan
 */

#ifndef STRING_PARSER_H_
#define STRING_PARSER_H_


#define _GUN_SOURCE

//this functions returns the number of tokens needed for the string array

typedef struct
{
    char** command_list;
    int num_token;
}command_line;

int count_token (char* buf, const char* delim);

//This functions can tokenize a string to token arrays, it returns the number
//of tokens in the string array, returns the token array as an argument
command_line str_filler (char* buf, const char* delim);

//this function safely free all the tokens within the array.
void free_command_line(command_line* command);



#endif /* STRING_PARSER_H_ */
